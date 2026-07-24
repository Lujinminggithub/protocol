#!/usr/bin/env python3
"""NB 三端指标采集、告警判定与自动故障包。"""
from __future__ import annotations

import datetime as dt
import json
import pathlib
import re
import shutil
import sys
import time

import deploy
import nb_diag


STATE_FILE = deploy.BUILD_DIR / "observe-state.json"
OUTPUT_DIR = deploy.BUILD_DIR / "observability"


def _remote_control_query_commands(connection, role: str, commands: tuple[str, ...]) -> list[dict]:
    script = f"""import glob,json,re,socket
paths=[p for p in sorted(glob.glob({f'/run/nb-{role}-*.ctl'!r})) if re.fullmatch({f'/run/nb-{role}-[0-9]+\\.ctl'!r},p)]
if not paths:raise RuntimeError('no control sockets for {role}')
results=[]
for path in paths:
    for command in {commands!r}:
        client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        client.settimeout(2)
        try:
            client.connect(path);client.sendall((command+'\\n').encode())
            response=json.loads(client.recv(16384).decode())
            results.append({{'path':path,'command':command,'response':response}})
        except Exception:
            if command in ('health','metrics'):raise
        finally:client.close()
print(json.dumps(results))
"""
    raw = deploy.run(connection, f"python3 -c {deploy.shlex.quote(script)}", tmo=20).strip()
    records = json.loads(raw)
    by_worker: dict[str, dict] = {}
    for record in records:
        response = record["response"]
        match = re.search(rf"/nb-{re.escape(role)}-(\d+)\.ctl$", record["path"])
        worker = str(response.get("worker", match.group(1) if match else "unknown"))
        by_worker.setdefault(worker, {})[record["command"]] = response
    return [{"role": role, "worker": worker, **values} for worker, values in sorted(by_worker.items())]


def _remote_control_query(connection, role: str) -> list[dict]:
    commands = ("health", "metrics", "tenants", "routes") if role == "entry" else ("health", "metrics")
    return _remote_control_query_commands(connection,role,commands)


def collect(attempts: int = 3, retry_delay: float = 0.75) -> dict:
    result = {
        "schema_version": 1,
        "collected_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "workers": [],
        "collection_errors": [],
    }
    for role in ("entry", "middle", "exit"):
        last_error = None
        for attempt in range(max(1, attempts)):
            connection = None
            try:
                connection = deploy.connect(role)
                result["workers"].extend(_remote_control_query(connection, role))
                last_error = None
                break
            except Exception as error:
                last_error = error
                if attempt + 1 < max(1, attempts):
                    time.sleep(deploy.ssh_retry_delay(attempt, max(0.0, retry_delay), 8.0))
            finally:
                if connection is not None: connection.close()
        if last_error is not None:
            result["collection_errors"].append({"role": role,
                "error": f"{type(last_error).__name__}: {last_error}", "attempts": max(1, attempts)})
    return result


def _delta(current: dict, previous: dict, path: tuple[str, ...]) -> float:
    def value(source):
        for key in path: source = source.get(key, {}) if isinstance(source, dict) else {}
        return float(source) if isinstance(source, (int, float)) else 0.0
    return max(0.0, value(current) - value(previous))


def evaluate(snapshot: dict, previous_state: dict | None = None) -> tuple[list[dict], dict]:
    previous_state = previous_state or {}
    previous_metrics = previous_state.get("metrics", {})
    old_streaks = previous_state.get("streaks", {})
    alerts = []
    current_metrics = {}

    def add(severity: str, code: str, scope: str, value, threshold, message: str):
        alerts.append({"severity": severity, "code": code, "scope": scope,
                       "value": value, "threshold": threshold, "message": message})

    for error in snapshot.get("collection_errors", []):
        add("critical", "node-unreachable", error["role"], error["error"], "reachable", "节点指标采集失败")

    releases = set()
    profiles = set()
    for worker in snapshot.get("workers", []):
        scope = f"{worker['role']}:{worker['worker']}"
        health = worker.get("health", {}); metrics = worker.get("metrics", {})
        current_metrics[scope] = metrics
        if health.get("status") != "ok": add("critical", "health-failed", scope, health, "status=ok", "worker 健康检查失败")
        releases.add(str(health.get("release_id", "missing")))
        profiles.add(f"{health.get('line_profile')}:{health.get('line_profile_schema')}")
        ages = metrics.get("queue_age_max_us", {})
        max_age = max((float(ages.get(name, 0)) for name in ("down", "up", "q2t")), default=0)
        if max_age >= 1_000_000: add("critical", "queue-age", scope, max_age, 1_000_000, "队列最老数据超过 1 秒")
        elif max_age >= 500_000: add("warning", "queue-age", scope, max_age, 500_000, "队列最老数据超过 500ms")
        link = metrics.get("link", {}); sent = int(link.get("sent_packets", 0) or 0)
        loss = float(link.get("effective_loss_max_pct", 0) or 0)
        if sent >= 20 and loss >= 8.0: add("critical", "effective-loss", scope, loss, 8.0, "有效丢包持续偏高")
        elif sent >= 20 and loss >= 3.0: add("warning", "effective-loss", scope, loss, 3.0, "有效丢包超过观察阈值")
        reorder_delay = int(link.get("reorder_delay_max_us", 0) or 0)
        if reorder_delay >= 1_000_000: add("critical", "reorder-delay", scope, reorder_delay, 1_000_000, "重排序延迟超过 1 秒")
        elif reorder_delay >= 450_000: add("warning", "reorder-delay", scope, reorder_delay, 450_000, "重排序延迟达到当前基线容忍上限")
        previous = previous_metrics.get(scope, {})
        lost_delta = _delta(metrics, previous, ("link", "lost_total"))
        spurious_delta = _delta(metrics, previous, ("link", "spurious_total"))
        if lost_delta >= 20 and spurious_delta/lost_delta >= 0.70:
            add("warning", "spurious-ratio", scope, round(spurious_delta/lost_delta, 3), 0.70, "丢包判断中伪丢包比例过高")
        udp_errors = _delta(metrics, previous, ("udp_errors", "rx")) + _delta(metrics, previous, ("udp_errors", "tx"))
        close_errors = _delta(metrics, previous, ("closed", "error"))
        loop_over20 = _delta(metrics, previous, ("event_loop", "over_20ms"))
        if udp_errors >= 10: add("critical", "udp-errors", scope, udp_errors, 10, "采样间隔内 UDP 错误过多")
        elif udp_errors > 0: add("warning", "udp-errors", scope, udp_errors, 1, "采样间隔内出现 UDP 错误")
        if close_errors >= 5: add("critical", "close-errors", scope, close_errors, 5, "采样间隔内异常关闭过多")
        if loop_over20 >= 5: add("critical", "event-loop-late", scope, loop_over20, 5, "事件循环多次阻塞超过 20ms")
        elif loop_over20 > 0: add("warning", "event-loop-late", scope, loop_over20, 1, "事件循环出现超过 20ms 阻塞")

    if len(releases) > 1: add("critical", "release-mismatch", "cluster", sorted(releases), "one release", "三端 release ID 不一致")
    if len(profiles) > 1: add("critical", "profile-mismatch", "cluster", sorted(profiles), "one profile", "三端线路 profile 不一致")

    active_keys = {f"{alert['code']}:{alert['scope']}" for alert in alerts}
    streaks = {key: old_streaks.get(key, 0) + 1 for key in active_keys}
    for alert in alerts: alert["streak"] = streaks[f"{alert['code']}:{alert['scope']}"]
    state = {"metrics": current_metrics, "streaks": streaks,
             "updated_at_utc": snapshot.get("collected_at_utc")}
    return alerts, state


def run_once(auto_bundle: bool = True) -> tuple[dict, list[dict]]:
    snapshot = collect()
    previous = json.loads(STATE_FILE.read_text(encoding="utf-8")) if STATE_FILE.is_file() else {}
    alerts, state = evaluate(snapshot, previous)
    snapshot["alerts"] = alerts
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = OUTPUT_DIR / f"metrics-{stamp}.json"
    output.write_text(json.dumps(snapshot, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    shutil.copy2(output, OUTPUT_DIR / "latest.json")
    last_bundle_at = float(previous.get("last_bundle_at", 0) or 0)
    trigger = any(a["severity"] == "critical" and (a["streak"] >= 2 or a["code"] in
        ("node-unreachable", "health-failed", "release-mismatch", "profile-mismatch")) for a in alerts)
    if auto_bundle and trigger and time.time()-last_bundle_at>=600:
        directory, archive, _ = nb_diag.incident_bundle()
        snapshot["incident_bundle"] = {"directory": str(directory), "archive": str(archive)}
        state["last_bundle_at"] = time.time()
        output.write_text(json.dumps(snapshot, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        shutil.copy2(output, OUTPUT_DIR / "latest.json")
    elif last_bundle_at:
        state["last_bundle_at"] = last_bundle_at
    STATE_FILE.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(snapshot, ensure_ascii=False, indent=2))
    print(f"指标快照: {output}")
    return snapshot, alerts


if __name__ == "__main__":
    run_once(auto_bundle="--no-bundle" not in sys.argv[1:])
