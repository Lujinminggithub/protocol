#!/usr/bin/env python3
"""对指定 NB QUIC 跳执行可回滚的 netem 矩阵。

故障在接收端通过 IFB 注入，只匹配 UDP/4443，不替换业务网卡根 qdisc，
因此 SSH 和同机其他线路不会进入 netem。默认仅做预检，必须显式传入 --apply。
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import shlex
import threading
import time
import os
from dataclasses import dataclass

import deploy
import line_probe
import nb_observe


FILTER_PREF = 49152
QUIC_PORT = 4443


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int((len(ordered) - 1) * p + 0.5)))
    return ordered[index]


def checked(connection, command: str, timeout: int = 30) -> str:
    marker = "__NB_RC__="
    output = deploy.run(connection, f"{command}; printf '\\n{marker}%s\\n' $?", tmo=timeout)
    lines = output.rstrip().splitlines()
    if not lines or not lines[-1].startswith(marker):
        raise RuntimeError(f"远端命令没有返回状态: {command}")
    rc = int(lines[-1][len(marker):])
    body = "\n".join(lines[:-1]).strip()
    if rc != 0:
        raise RuntimeError(f"远端命令失败 rc={rc}: {command}\n{body}")
    return body


def segment_target(name: str) -> dict:
    if name == "middle-exit":
        receiver = deploy._role_host("exit")
        source = deploy._role_host("middle")
        return {
            "source_role": "middle",
            "receiver_role": "exit",
            "source_ip": source["host"],
            "destination_ip": receiver["host"],
        }
    middle = deploy._role_host("middle")
    return {
        "source_role": "entry",
        "receiver_role": "middle",
        "source_ip": deploy._role_host("entry")["host"],
        "destination_ip": middle.get("private_ip") or middle["host"],
    }


@dataclass
class SelectiveNetem:
    receiver_role: str
    source_ip: str
    destination_ip: str
    port: int = QUIC_PORT
    port_direction: str = "dst"
    filter_pref: int = FILTER_PREF
    connection: object | None = None
    interface: str = ""
    ifb: str = ""
    created_clsact: bool = False
    installed: bool = False
    guard_pid: int = 0

    def open(self) -> dict:
        if self.port_direction not in ("src", "dst"):
            raise RuntimeError(f"invalid netem port direction: {self.port_direction}")
        self.connection = deploy.connect(self.receiver_role)
        local_lookup = (
            f"ip -o -4 addr show | awk -v ip={shlex.quote(self.destination_ip)} "
            "'$4 ~ (\"^\" ip \"/\") {print $2; exit}'"
        )
        self.interface = deploy.run(self.connection, local_lookup).strip()
        if not self.interface:
            route = checked(self.connection, f"ip route get {shlex.quote(self.source_ip)}")
            fields = route.split()
            self.interface = fields[fields.index("dev") + 1] if "dev" in fields else ""
        if not self.interface or any(not (c.isalnum() or c in "_.-") for c in self.interface):
            raise RuntimeError(f"无法识别 {self.receiver_role} 接收网卡: {self.interface!r}")
        self.ifb = f"ifbnb{self.receiver_role[0]}0"
        checked(self.connection, "command -v tc >/dev/null && command -v ip >/dev/null")
        filters = deploy.run(
            self.connection,
            f"tc filter show dev {shlex.quote(self.interface)} ingress pref {self.filter_pref} 2>/dev/null || true",
        )
        if filters.strip():
            raise RuntimeError(f"过滤器 pref={self.filter_pref} 已被占用，拒绝覆盖")
        if deploy.run(self.connection, f"ip link show {self.ifb} 2>/dev/null || true").strip():
            raise RuntimeError(f"IFB 设备 {self.ifb} 已存在，拒绝复用或删除外部设备")
        qdisc = deploy.run(self.connection, f"tc qdisc show dev {shlex.quote(self.interface)}")
        return {
            "receiver_role": self.receiver_role,
            "interface": self.interface,
            "source_ip": self.source_ip,
            "destination_ip": self.destination_ip,
            "udp_port": self.port,
            "port_direction": self.port_direction,
            "original_qdisc": qdisc.strip(),
        }

    def install(self) -> None:
        assert self.connection is not None and self.interface and self.ifb
        has_clsact = "clsact" in deploy.run(
            self.connection, f"tc qdisc show dev {shlex.quote(self.interface)}"
        )
        checked(self.connection, "modprobe ifb numifbs=4")
        checked(self.connection, f"ip link add {self.ifb} type ifb")
        checked(self.connection, f"ip link set dev {self.ifb} up")
        if not has_clsact:
            checked(self.connection, f"tc qdisc add dev {self.interface} clsact")
            self.created_clsact = True
        checked(
            self.connection,
            f"tc filter add dev {self.interface} ingress protocol ip pref {self.filter_pref} flower "
            f"ip_proto udp {self.port_direction}_port {self.port} action mirred egress redirect dev {self.ifb}",
        )
        self.installed = True
        cleanup = (
            f"tc filter del dev {self.interface} ingress protocol ip pref {self.filter_pref} 2>/dev/null || true; "
            f"tc qdisc del dev {self.ifb} root 2>/dev/null || true; "
            f"ip link del dev {self.ifb} 2>/dev/null || true"
        )
        if self.created_clsact:
            cleanup += f"; tc qdisc del dev {self.interface} clsact 2>/dev/null || true"
        raw_pid = checked(
            self.connection,
            f"nohup setsid sh -c {shlex.quote('sleep 14400; ' + cleanup)} >/dev/null 2>&1 & echo $!",
        )
        self.guard_pid = int(raw_pid.splitlines()[-1])

    def configure(self, loss_pct: float, delay_ms: int, jitter_ms: int, reorder_pct: float) -> str:
        assert self.connection is not None and self.installed
        parts = [f"delay {delay_ms}ms"]
        if jitter_ms:
            parts[-1] += f" {jitter_ms}ms distribution normal"
        parts.append(f"loss random {loss_pct}%")
        if reorder_pct:
            parts.append(f"reorder {reorder_pct}% 50%")
        checked(self.connection, f"tc qdisc replace dev {self.ifb} root netem {' '.join(parts)}")
        return checked(self.connection, f"tc -s qdisc show dev {self.ifb}")

    def stats(self) -> str:
        if self.connection is None or not self.ifb:
            return ""
        return deploy.run(self.connection, f"tc -s qdisc show dev {self.ifb} 2>/dev/null || true").strip()

    def close(self) -> None:
        if self.connection is None:
            return
        try:
            if self.guard_pid:
                deploy.run(self.connection, f"kill -- -{self.guard_pid} 2>/dev/null || true")
            if self.interface:
                deploy.run(
                    self.connection,
                    f"tc filter del dev {self.interface} ingress protocol ip pref {self.filter_pref} 2>/dev/null || true",
                )
                if self.created_clsact:
                    deploy.run(self.connection, f"tc qdisc del dev {self.interface} clsact 2>/dev/null || true")
            if self.ifb:
                deploy.run(self.connection, f"tc qdisc del dev {self.ifb} root 2>/dev/null || true")
                deploy.run(self.connection, f"ip link del dev {self.ifb} 2>/dev/null || true")
        finally:
            self.installed = False
            self.guard_pid = 0
            self.connection.close()
            self.connection = None


def summarize_observation(samples: list[dict]) -> dict:
    queue_ages: list[float] = []
    losses: list[float] = []
    reorder_delays: list[float] = []
    collection_errors: list[dict] = []
    unhealthy: list[str] = []
    deployments: set[str] = set()
    profiles: set[str] = set()
    for sample in samples:
        collection_errors.extend(sample.get("collection_errors", []))
        for worker in sample.get("workers", []):
            scope = f"{worker.get('role')}:{worker.get('worker')}"
            if worker.get("health", {}).get("status") != "ok":
                unhealthy.append(scope)
            health = worker.get("health", {})
            deployments.add(str(health.get("release_id", "missing")))
            profiles.add(f"{health.get('line_profile', 'missing')}:{health.get('line_profile_schema', 'missing')}")
            metrics = worker.get("metrics", {})
            ages = metrics.get("queue_age_max_us", {})
            queue_ages.append(max((float(ages.get(k, 0) or 0) for k in ("down", "up", "q2t")), default=0))
            link = metrics.get("link", {})
            losses.append(float(link.get("effective_loss_max_pct", 0) or 0))
            reorder_delays.append(float(link.get("reorder_delay_max_us", 0) or 0))
    return {
        "samples": len(samples),
        "collection_errors": collection_errors,
        "unhealthy_workers": sorted(set(unhealthy)),
        "deployments": sorted(deployments),
        "profiles": sorted(profiles),
        "cluster_mismatch": len(deployments) != 1 or len(profiles) != 1,
        "queue_age_p95_us": percentile(queue_ages, 0.95),
        "queue_age_max_us": max(queue_ages, default=0.0),
        "effective_loss_p95_pct": percentile(losses, 0.95),
        "reorder_delay_p95_us": percentile(reorder_delays, 0.95),
    }


def validate_cluster(snapshot: dict) -> None:
    if snapshot.get("collection_errors"):
        raise RuntimeError(f"三端采集失败: {snapshot['collection_errors']}")
    roles = {worker.get("role") for worker in snapshot.get("workers", [])}
    if roles != {"entry", "middle", "exit"}:
        raise RuntimeError(f"三端 worker 不完整: {sorted(str(role) for role in roles)}")
    deployments = set()
    profiles = set()
    for worker in snapshot["workers"]:
        scope = f"{worker.get('role')}:{worker.get('worker')}"
        health = worker.get("health", {})
        if health.get("status") != "ok":
            raise RuntimeError(f"{scope} health 非正常")
        deployments.add(str(health.get("release_id", "missing")))
        profiles.add(f"{health.get('line_profile', 'missing')}:{health.get('line_profile_schema', 'missing')}")
        if int(worker.get("metrics", {}).get("fec", {}).get("active", 0) or 0) != 0:
            raise RuntimeError(f"{scope} FEC active 非 0，拒绝混合执行基线矩阵")
    if len(deployments) != 1 or len(profiles) != 1:
        raise RuntimeError(f"三端版本或 profile 不一致: deployment={deployments}, profile={profiles}")


def entry_resource_snapshot() -> dict:
    connection = deploy.connect("entry")
    try:
        raw = deploy.run(connection,
            "awk '/MemAvailable:/ {print \"mem_kb=\" $2}' /proc/meminfo; "
            "echo sshd=$(pgrep -c sshd 2>/dev/null || echo 0); "
            "awk '/oom_kill / {print \"oom_kill=\" $2}' /proc/vmstat")
    finally:
        connection.close()
    result = {}
    for line in raw.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            if value.strip().isdigit():
                result[key.strip()] = int(value.strip())
    return result


def validate_resources(current: dict, baseline: dict, min_mem_mb: int, max_sshd: int) -> None:
    if current.get("mem_kb", 0) < min_mem_mb * 1024:
        raise RuntimeError(f"entry 可用内存低于 {min_mem_mb}MB: {current}")
    if current.get("sshd", 0) > max_sshd:
        raise RuntimeError(f"entry sshd 数量超过 {max_sshd}: {current}")
    if current.get("oom_kill", 0) > baseline.get("oom_kill", 0):
        raise RuntimeError(f"矩阵期间发生新 OOM: baseline={baseline}, current={current}")


def run_case(args, loss_pct: float, injected: bool = True) -> dict:
    entry_host = deploy._role_host("entry")["host"]
    result: dict = {
        "loss_pct": loss_pct,
        "delay_ms": args.delay_ms if injected else 0,
        "jitter_ms": args.jitter_ms if injected else 0,
        "reorder_pct": args.reorder_pct if injected else 0.0,
        "injected": injected,
    }
    holder: dict = {}

    def load() -> None:
        try:
            holder["load"] = line_probe.run_load_probe(
                entry_host, args.socks_port, args.target_mbps, args.duration
            )
        except Exception as error:  # propagated through the result for cleanup safety
            holder["error"] = f"{type(error).__name__}: {error}"

    result["integrity_before"] = line_probe.run_integrity_probe(entry_host, args.socks_port, args.integrity_bytes)
    thread = threading.Thread(target=load, name="nb-netem-load", daemon=True)
    thread.start()
    samples = []
    while thread.is_alive():
        samples.append(nb_observe.collect())
        thread.join(timeout=args.interval)
    samples.append(nb_observe.collect())
    result.update(holder)
    result["observation"] = summarize_observation(samples)
    result["integrity_after"] = line_probe.run_integrity_probe(entry_host, args.socks_port, args.integrity_bytes)
    reasons = []
    if "error" in holder:
        reasons.append("load-error")
    if result["integrity_before"]["integrity"] != "ok" or result["integrity_after"]["integrity"] != "ok":
        reasons.append("payload-integrity")
    if result["observation"]["collection_errors"]:
        reasons.append("collection-error")
    if result["observation"]["unhealthy_workers"]:
        reasons.append("worker-unhealthy")
    if result["observation"]["cluster_mismatch"]:
        reasons.append("cluster-mismatch")
    if result["observation"]["queue_age_p95_us"] > args.max_queue_age_ms * 1000:
        reasons.append("queue-age")
    achieved = float(result.get("load", {}).get("achieved_mbps", 0) or 0)
    if achieved < args.target_mbps * args.min_throughput_ratio:
        reasons.append("throughput")
    result["failure_reasons"] = reasons
    result["passed"] = not reasons
    return result


def parse_losses(raw: str) -> list[float]:
    values = [float(item.strip()) for item in raw.split(",") if item.strip()]
    if not values or any(value < 0 or value > 30 for value in values):
        raise ValueError("loss 必须是 0..30 的逗号分隔百分比")
    return values


def load_scenarios(path: pathlib.Path | None, losses: list[float], args,
                   segment: str | None = None) -> list[dict]:
    if path is None:
        return [{"name": f"loss-{loss:g}", "category": "loss", "loss_pct": loss,
                 "delay_ms": args.delay_ms, "jitter_ms": args.jitter_ms,
                 "reorder_pct": args.reorder_pct} for loss in losses]
    value = json.loads(path.read_text(encoding="utf-8"))
    raw = value.get("scenarios") if isinstance(value, dict) else value
    if not isinstance(raw, list) or not raw:
        raise ValueError("场景文件必须包含非空 scenarios 数组")
    scenarios = []
    names = set()
    for index, item in enumerate(raw, 1):
        if not isinstance(item, dict):
            raise ValueError(f"场景 {index} 必须是对象")
        scenario = {
            "name": str(item.get("name") or f"case-{index}"),
            "category": str(item.get("category") or "custom"),
            "loss_pct": float(item.get("loss_pct", 0)),
            "delay_ms": int(item.get("delay_ms", 0)),
            "jitter_ms": int(item.get("jitter_ms", 0)),
            "reorder_pct": float(item.get("reorder_pct", 0)),
            "expected": str((item.get("expected_by_segment") or {}).get(
                segment, item.get("expected", "pass"))),
        }
        if scenario["name"] in names:
            raise ValueError(f"场景名称重复: {scenario['name']}")
        names.add(scenario["name"])
        if not 0 <= scenario["loss_pct"] <= 30:
            raise ValueError(f"{scenario['name']} loss_pct 越界")
        if not 0 <= scenario["delay_ms"] <= 2000 or not 0 <= scenario["jitter_ms"] <= 1000:
            raise ValueError(f"{scenario['name']} delay/jitter 越界")
        if not 0 <= scenario["reorder_pct"] <= 50:
            raise ValueError(f"{scenario['name']} reorder_pct 越界")
        if scenario["reorder_pct"] and scenario["delay_ms"] == 0:
            raise ValueError(f"{scenario['name']} 使用 reorder 时必须设置 delay_ms")
        if scenario["expected"] not in ("pass", "reject"):
            raise ValueError(f"{scenario['name']} expected 必须为 pass 或 reject")
        scenarios.append(scenario)
    return scenarios


def contract_met(case: dict, expected: str) -> bool:
    if expected == "pass":
        return bool(case.get("passed"))
    if case.get("passed"):
        return False
    reasons = set(case.get("failure_reasons", []))
    operational = {"collection-error", "worker-unhealthy", "cluster-mismatch", "resource-guard"}
    if reasons & operational:
        return False
    if reasons == {"probe-error"}:
        return "TimeoutError" in str(case.get("error", ""))
    return bool(reasons & {"throughput", "queue-age", "payload-integrity"})


def atomic_report(path: pathlib.Path, report: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + f".{os.getpid()}.tmp")
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for attempt in range(5):
        try:
            os.replace(temporary, path)
            return
        except OSError:
            if attempt == 4:
                raise
            time.sleep(0.1 * (attempt + 1))


def summarize_cases(cases: list[dict]) -> list[dict]:
    grouped: dict[tuple[str, str, float, int, int, float], list[dict]] = {}
    for case in cases:
        key = (str(case.get("scenario", "custom")), str(case.get("category", "custom")),
               float(case["loss_pct"]), int(case["delay_ms"]),
               int(case["jitter_ms"]), float(case["reorder_pct"]))
        grouped.setdefault(key, []).append(case)
    result = []
    for (scenario, category, loss, delay, jitter, reorder), group in grouped.items():
        throughputs = [float(item.get("load", {}).get("achieved_mbps", 0) or 0) for item in group]
        queue_ages = [float(item.get("observation", {}).get("queue_age_p95_us", 0) or 0) for item in group]
        passed = sum(bool(item.get("passed")) for item in group)
        result.append({
            "scenario": scenario,
            "category": category,
            "loss_pct": loss,
            "delay_ms": delay,
            "jitter_ms": jitter,
            "reorder_pct": reorder,
            "rounds": len(group),
            "passed_rounds": passed,
            "pass_rate": passed / len(group),
            "throughput_p50_mbps": percentile(throughputs, 0.50),
            "throughput_min_mbps": min(throughputs, default=0.0),
            "queue_age_p95_of_rounds_us": percentile(queue_ages, 0.95),
        })
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="NB 三跳真实 QUIC 定向 netem 矩阵")
    parser.add_argument("--segment", choices=("entry-middle", "middle-exit"), default="middle-exit")
    parser.add_argument("--loss", default="0,1,2,3,5,8")
    parser.add_argument("--scenario-file", type=pathlib.Path)
    parser.add_argument("--delay-ms", type=int, default=80)
    parser.add_argument("--jitter-ms", type=int, default=0)
    parser.add_argument("--reorder-pct", type=float, default=0.0)
    parser.add_argument("--duration", type=int, default=70)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--settle", type=int, default=15)
    parser.add_argument("--recovery", type=int, default=20)
    parser.add_argument("--interval", type=int, default=10)
    parser.add_argument("--target-mbps", type=float, default=10.0)
    parser.add_argument("--min-throughput-ratio", type=float, default=0.80)
    parser.add_argument("--integrity-bytes", type=int, default=256 * 1024)
    parser.add_argument("--max-queue-age-ms", type=int, default=1000)
    parser.add_argument("--min-entry-memory-mb", type=int, default=256)
    parser.add_argument("--max-entry-sshd", type=int, default=64)
    parser.add_argument("--socks-port", type=int, default=1080)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--resume", action="store_true",
                        help="Resume an incomplete matrix from --output")
    parser.add_argument("--apply", action="store_true", help="确认在 KZ 测试线注入定向故障")
    parser.add_argument("--continue-on-failure", action="store_true", help="失败后仍继续更高退化档")
    args = parser.parse_args()
    if args.duration < 20 or args.duration > 1800:
        parser.error("--duration 必须在 20..1800 秒")
    if args.rounds < 1 or args.rounds > 20:
        parser.error("--rounds 必须在 1..20")
    if min(args.delay_ms, args.jitter_ms, args.settle, args.recovery, args.interval) < 0:
        parser.error("时间参数不能为负数")
    if args.min_throughput_ratio <= 0 or args.min_throughput_ratio > 1:
        parser.error("--min-throughput-ratio 必须在 0..1")
    losses = parse_losses(args.loss)
    try:
        scenarios = load_scenarios(args.scenario_file, losses, args, args.segment)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    target = segment_target(args.segment)
    injector = SelectiveNetem(
        receiver_role=target["receiver_role"],
        source_ip=target["source_ip"],
        destination_ip=target["destination_ip"],
    )
    output = args.output or deploy.BUILD_DIR / "netem" / f"matrix-{dt.datetime.now(dt.timezone.utc):%Y%m%dT%H%M%SZ}.json"
    report = {
        "schema_version": 2,
        "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "line": "-".join(deploy._role_host(role)["name"] for role in ("entry", "middle", "exit")),
        "segment": args.segment,
        "fec_required_mode": "observe-only",
        "scenarios": scenarios,
        "cases": [],
    }
    if args.resume:
        if not output.exists():
            parser.error("--resume requires an existing --output report")
        previous = json.loads(output.read_text(encoding="utf-8"))
        if previous.get("segment") != args.segment:
            parser.error("resume report segment does not match --segment")
        previous_scenarios = [item.get("name") for item in previous.get("scenarios", [])]
        if previous_scenarios != [item.get("name") for item in scenarios]:
            parser.error("resume report scenarios do not match --scenario-file")
        report = previous
        for terminal_key in ("fatal_error", "finished_at_utc", "recovery", "status",
                             "stopped_after_failure", "stopped_after_resource_guard"):
            report.pop(terminal_key, None)
        report["resumed_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    completed_cases = {
        (case.get("scenario"), int(case.get("round", 0)))
        for case in report.get("cases", [])
    }
    try:
        report["preflight"] = injector.open()
        baseline = nb_observe.collect()
        validate_cluster(baseline)
        report["baseline"] = summarize_observation([baseline])
        resource_baseline = entry_resource_snapshot()
        validate_resources(resource_baseline, resource_baseline,
                           args.min_entry_memory_mb, args.max_entry_sshd)
        report["resource_baseline"] = resource_baseline
        print(json.dumps({"preflight": report["preflight"]}, ensure_ascii=False))
        if not args.apply:
            report["status"] = "preflight-only"
            print("预检通过；未修改线路。执行真实矩阵必须显式增加 --apply。")
        else:
            injector.install()
            stopped = False
            for scenario in scenarios:
                args.delay_ms = scenario["delay_ms"]
                args.jitter_ms = scenario["jitter_ms"]
                args.reorder_pct = scenario["reorder_pct"]
                loss = scenario["loss_pct"]
                for round_index in range(1, args.rounds + 1):
                    if (scenario["name"], round_index) in completed_cases:
                        continue
                    injector.configure(loss, args.delay_ms, args.jitter_ms, args.reorder_pct)
                    time.sleep(args.settle)
                    try:
                        case = run_case(args, loss)
                    except Exception as error:
                        case = {
                            "loss_pct": loss,
                            "delay_ms": args.delay_ms,
                            "jitter_ms": args.jitter_ms,
                            "reorder_pct": args.reorder_pct,
                            "injected": True,
                            "passed": False,
                            "failure_reasons": ["probe-error"],
                            "error": f"{type(error).__name__}: {error}",
                        }
                    case["round"] = round_index
                    case["scenario"] = scenario["name"]
                    case["category"] = scenario["category"]
                    case["expected"] = scenario["expected"]
                    case["contract_met"] = contract_met(case, scenario["expected"])
                    case["netem_stats"] = injector.stats()
                    report["cases"].append(case)
                    resources = entry_resource_snapshot()
                    case["entry_resources"] = resources
                    try:
                        validate_resources(resources, resource_baseline,
                                           args.min_entry_memory_mb, args.max_entry_sshd)
                    except RuntimeError as resource_error:
                        case["passed"] = False
                        case.setdefault("failure_reasons", []).append("resource-guard")
                        case["resource_error"] = str(resource_error)
                        stopped = True
                    report["summary"] = summarize_cases(report["cases"])
                    atomic_report(output, report)
                    print(json.dumps(case, ensure_ascii=False))
                    if not case["passed"] and not args.continue_on_failure:
                        report["stopped_after_failure"] = {"loss_pct": loss, "round": round_index}
                        stopped = True
                        break
                    if stopped:
                        report["stopped_after_resource_guard"] = {
                            "scenario": scenario["name"], "round": round_index}
                        break
                if stopped:
                    break
    except Exception as error:
        report["fatal_error"] = f"{type(error).__name__}: {error}"
    finally:
        injector.close()

    if args.apply:
        time.sleep(args.recovery)
        try:
            report["recovery"] = run_case(args, 0.0, injected=False)
        except Exception as error:
            report["recovery"] = {"passed": False, "error": f"{type(error).__name__}: {error}"}
    report["finished_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    expectations = {item["name"]: item.get("expected", "pass") for item in scenarios}
    for case in report["cases"]:
        case["expected"] = expectations.get(case.get("scenario"), "pass")
        case["contract_met"] = contract_met(case, case["expected"])
    report["summary"] = summarize_cases(report["cases"])
    if args.apply:
        report["status"] = "passed" if (
            not report.get("fatal_error")
            and all(case.get("contract_met") for case in report["cases"])
            and report.get("recovery", {}).get("passed")
        ) else "failed"
    elif report.get("fatal_error"):
        report["status"] = "failed"
    atomic_report(output, report)
    print(f"矩阵报告: {output}")
    return 0 if report["status"] in ("passed", "preflight-only") else 1


if __name__ == "__main__":
    raise SystemExit(main())
