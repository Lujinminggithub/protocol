#!/usr/bin/env python3
"""Collect one registry line over its existing SSH topology for nb-web-worker."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import shlex

import deploy


def safe_id(value: object, fallback: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "-", str(value or "")).strip("-.")
    return (cleaned or fallback)[:64]


def profile_id(health: dict) -> str:
    profile = str(health.get("line_profile") or "")
    schema = health.get("line_profile_schema")
    if profile and schema is not None and ":" not in profile:
        return f"{profile}:{schema}"
    return profile


def number(source: dict, key: str) -> float:
    value = source.get(key, 0)
    return float(value) if isinstance(value, (int, float)) else 0.0


def snapshot(line_id: str, role: str, observed: str, record: dict) -> dict:
    health = record.get("health") if isinstance(record.get("health"), dict) else {}
    metrics = record.get("metrics") if isinstance(record.get("metrics"), dict) else {}
    error = str(record.get("error") or "")
    path = str(record.get("path") or "")
    prefix = deploy._control_socket_prefix(role)
    node_id = safe_id(pathlib.PurePosixPath(path).stem, f"{prefix}-collector")
    worker = safe_id(health.get("worker"), "collector")
    status = str(health.get("status") or "down")
    if error or status not in ("ok", "degraded", "down"):
        status = "down"
    ages = metrics.get("queue_age_max_us") if isinstance(metrics.get("queue_age_max_us"), dict) else {}
    link = metrics.get("link") if isinstance(metrics.get("link"), dict) else {}
    fec = metrics.get("fec") if isinstance(metrics.get("fec"), dict) else {}
    byte_counts = metrics.get("bytes") if isinstance(metrics.get("bytes"), dict) else {}
    udp_errors = metrics.get("udp_errors") if isinstance(metrics.get("udp_errors"), dict) else {}
    return {
        "line_id": line_id,
        "node_id": node_id,
        "role": role,
        "worker_id": worker,
        "observed_at": observed,
        "health": status,
        "deployment": str(health.get("release_id") or ""),
        "profile": profile_id(health),
        "sessions": int(number(metrics, "sessions") or number(metrics, "sessions_inuse")),
        "throughput_mbps": number(metrics, "throughput_mbps"),
        "queue_age_p95_us": max((number(ages, key) for key in ("down", "up", "q2t")), default=0.0),
        "effective_loss_pct": number(link, "effective_loss_max_pct"),
        "fec_observe": number(fec, "observe") != 0,
        "fec_active": number(fec, "active") != 0,
        "payload": {"health": health, "metrics": metrics, "collection_error": error},
        "_bytes_c2s": number(byte_counts, "c2s"),
        "_bytes_s2c": number(byte_counts, "s2c"),
        "_rxq_overflow_total": number(udp_errors, "rxq_overflow"),
    }


def apply_throughput(samples: list[dict], state_file: pathlib.Path | None) -> None:
    previous = {}
    if state_file and state_file.is_file():
        try:
            loaded = json.loads(state_file.read_text(encoding="utf-8"))
            previous = loaded if isinstance(loaded, dict) else {}
        except (OSError, ValueError):
            previous = {}
    current = {}
    for item in samples:
        c2s = float(item.pop("_bytes_c2s", 0))
        s2c = float(item.pop("_bytes_s2c", 0))
        rxq_total = float(item.pop("_rxq_overflow_total", 0))
        prior = previous.get(item["node_id"], {})
        try:
            before = dt.datetime.fromisoformat(str(prior["observed_at"]).replace("Z", "+00:00"))
            after = dt.datetime.fromisoformat(item["observed_at"].replace("Z", "+00:00"))
            elapsed = (after - before).total_seconds()
            up_delta = c2s - float(prior["bytes_c2s"])
            down_delta = s2c - float(prior["bytes_s2c"])
            if 0 < elapsed <= 300 and up_delta >= 0 and down_delta >= 0:
                item["upstream_mbps"] = up_delta * 8 / elapsed / 1_000_000
                item["downstream_mbps"] = down_delta * 8 / elapsed / 1_000_000
                item["throughput_mbps"] = item["upstream_mbps"] + item["downstream_mbps"]
        except (KeyError, TypeError, ValueError):
            pass
        previous_rxq = float(prior.get("rxq_overflow_total", 0) or 0)
        rxq_delta = rxq_total-previous_rxq if rxq_total>=previous_rxq else rxq_total
        if rxq_delta>0:
            if item["health"] == "ok":
                item["health"] = "degraded"
            item["payload"]["rxq_overflow_delta"] = rxq_delta
        item.setdefault("upstream_mbps", 0.0);item.setdefault("downstream_mbps", 0.0)
        current[item["node_id"]] = {"observed_at": item["observed_at"], "bytes_c2s": c2s,
                                    "bytes_s2c": s2c,
                                    "rxq_overflow_total": rxq_total}
    if state_file:
        state_file.parent.mkdir(parents=True, exist_ok=True)
        temporary = state_file.with_suffix(state_file.suffix + ".new")
        temporary.write_text(json.dumps(current, separators=(",", ":")) + "\n", encoding="utf-8")
        temporary.replace(state_file)


def query_role(role: str) -> list[dict]:
    pattern = deploy._control_socket_glob(role)
    prefix = deploy._control_socket_prefix(role)
    socket_pattern = f"/run/{prefix}-[0-9]+\\.ctl"
    script = f"""import glob,json,re,socket
results=[]
paths=[path for path in sorted(glob.glob({pattern!r}))
       if re.fullmatch({socket_pattern!r},path)]
if not paths:
    raise RuntimeError('no matching control sockets')
for path in paths:
    item={{'path':path}}
    try:
        for command in ('health','metrics'):
            client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
            client.settimeout(3)
            try:
                client.connect(path)
                client.sendall((command+'\\n').encode())
                item[command]=json.loads(client.recv(65536).decode())
            finally:
                client.close()
    except Exception as error:
        item['error']=type(error).__name__+': '+str(error)
    results.append(item)
print(json.dumps(results,separators=(',',':')))
"""
    connection = deploy.connect(role)
    try:
        output = deploy.checked_run(connection, f"python3 -c {shlex.quote(script)}", tmo=20)
        records = json.loads(output.strip())
        if not isinstance(records, list):
            raise RuntimeError("invalid remote snapshot response")
        return records
    finally:
        connection.close()


def collect(line_id: str) -> list[dict]:
    observed = dt.datetime.now(dt.timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")
    result = []
    for role in ("entry", "middle", "exit"):
        try:
            records = query_role(role)
        except Exception:  # A failed role must not suppress healthy roles or create a fake node.
            continue
        result.extend(snapshot(line_id, role, observed, record) for record in records)
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--line-id", required=True)
    parser.add_argument("--state-file", type=pathlib.Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", args.line_id):
        raise SystemExit("invalid line id")
    samples = collect(args.line_id)
    apply_throughput(samples, args.state_file)
    print("SNAPSHOTS_JSON=" + json.dumps(samples, separators=(",", ":")))


if __name__ == "__main__":
    main()
