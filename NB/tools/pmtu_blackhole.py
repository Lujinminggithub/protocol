#!/usr/bin/env python3
"""Controlled PMTU blackhole test on the KZ middle-to-exit segment."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import shlex
import threading
import time

import deploy
import line_probe
import nb_observe
from netem_matrix import atomic_report, checked, summarize_observation, validate_cluster


COMMENT = "nb-p2-pmtu-blackhole"
QUIC_PORT = 4443


def pmtu_fallbacks(snapshot: dict) -> int:
    return sum(int(worker.get("metrics", {}).get("pmtu", {}).get("fallbacks", 0) or 0)
               for worker in snapshot.get("workers", []))


def current_mtu(role: str) -> int:
    connection = deploy.connect(role)
    try:
        path = line_probe.remote_log_path(connection, role)
        line = deploy.run(connection, f"grep 'linkq pool' {shlex.quote(path)} | tail -1")
    finally:
        connection.close()
    match = re.search(r"\bmtu=(\d+)\b", line)
    return int(match.group(1)) if match else 0


class SizeBlackhole:
    def __init__(self, max_ip_length: int):
        self.max_ip_length = max_ip_length
        self.connection = None
        self.guard_pid = 0
        self.installed = False

    @property
    def rule(self) -> str:
        return (f"-p udp --dport {QUIC_PORT} -m length "
                f"--length {self.max_ip_length + 1}:65535 "
                f"-m comment --comment {COMMENT} -j DROP")

    def open(self) -> None:
        self.connection = deploy.connect("exit")
        checked(self.connection, "command -v iptables >/dev/null")
        checked(self.connection,
            f"if iptables -C INPUT {self.rule} 2>/dev/null; then false; else true; fi")

    def install(self) -> None:
        assert self.connection is not None
        checked(self.connection, f"iptables -I INPUT 1 {self.rule}")
        self.installed = True
        cleanup = f"iptables -D INPUT {self.rule} 2>/dev/null || true"
        raw = checked(self.connection,
            f"nohup setsid sh -c {shlex.quote('sleep 90; ' + cleanup)} "
            ">/dev/null 2>&1 & echo $!")
        self.guard_pid = int(raw.splitlines()[-1])

    def close(self) -> None:
        if self.connection is None:
            return
        try:
            if self.installed:
                deploy.run(self.connection, f"iptables -D INPUT {self.rule} 2>/dev/null || true")
            if self.guard_pid:
                deploy.run(self.connection, f"kill -- -{self.guard_pid} 2>/dev/null || true")
        finally:
            self.connection.close()
            self.connection = None
            self.installed = False
            self.guard_pid = 0


def run(args) -> dict:
    before = nb_observe.collect()
    validate_cluster(before)
    workers = before["workers"]
    deployments = sorted({str(item["health"]["release_id"]) for item in workers})
    profiles = sorted({f"{item['health']['line_profile']}:{item['health']['line_profile_schema']}"
                       for item in workers})
    report = {
        "schema_version": 1,
        "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "segment": "middle-exit",
        "max_ip_length": args.max_ip_length,
        "deployment": deployments[0],
        "profile": profiles[0],
        "status": "running",
        "mtu_before": current_mtu("middle"),
        "integrity_before": line_probe.run_integrity_probe(
            deploy._role_host("entry")["host"], args.socks_port, args.integrity_bytes),
    }
    atomic_report(args.output, report)
    holder: dict = {}

    def load() -> None:
        try:
            holder["load"] = line_probe.run_load_probe(
                deploy._role_host("entry")["host"], args.socks_port,
                args.target_mbps, args.duration, io_timeout=args.duration + 30)
        except Exception as error:
            holder["error"] = f"{type(error).__name__}: {error}"

    blackhole = SizeBlackhole(args.max_ip_length)
    samples = [before]
    thread = threading.Thread(target=load, name="nb-pmtu-blackhole-load", daemon=True)
    thread.start()
    time.sleep(args.warmup)
    injected_at = time.monotonic()
    baseline_fallbacks = pmtu_fallbacks(before)
    recovery_ms = 0.0
    try:
        blackhole.open()
        blackhole.install()
        report["fault_injected"] = True
        deadline = time.monotonic() + args.inject_duration
        while time.monotonic() < deadline and thread.is_alive():
            sample = nb_observe.collect()
            samples.append(sample)
            if recovery_ms == 0 and pmtu_fallbacks(sample) > baseline_fallbacks:
                recovery_ms = (time.monotonic() - injected_at) * 1000.0
            time.sleep(args.interval)
    except Exception as error:
        holder["error"] = f"{type(error).__name__}: {error}"
    finally:
        blackhole.close()
    thread.join(args.duration + 30)
    if thread.is_alive():
        holder["error"] = "load thread exceeded timeout"
    samples.append(nb_observe.collect())
    report.update(holder)
    report["recovery_ms"] = round(recovery_ms, 3)
    report["mtu_after"] = current_mtu("middle")
    report["observation"] = summarize_observation(samples)
    report["integrity_after"] = line_probe.run_integrity_probe(
        deploy._role_host("entry")["host"], args.socks_port, args.integrity_bytes)
    reasons = []
    if holder.get("error"):
        reasons.append("load-error")
    if report.get("load", {}).get("integrity") != "count-ok":
        reasons.append("logical-flow-interrupted")
    if report["integrity_before"]["integrity"] != "ok" or report["integrity_after"]["integrity"] != "ok":
        reasons.append("payload-integrity")
    if not (report["mtu_before"] > report["mtu_after"] >= 1200) or recovery_ms <= 0:
        reasons.append("mtu-fallback-not-observed")
    observation = report["observation"]
    if observation["collection_errors"]:
        reasons.append("collection-error")
    if observation["unhealthy_workers"]:
        reasons.append("worker-unhealthy")
    if observation["cluster_mismatch"]:
        reasons.append("cluster-mismatch")
    report["failure_reasons"] = reasons
    report["status"] = "passed" if not reasons else "failed"
    report["finished_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    atomic_report(args.output, report)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Inject a guarded PMTU blackhole")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--max-ip-length", type=int, default=1320)
    parser.add_argument("--duration", type=int, default=45)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--inject-duration", type=int, default=25)
    parser.add_argument("--interval", type=int, default=2)
    parser.add_argument("--target-mbps", type=float, default=4.0)
    parser.add_argument("--integrity-bytes", type=int, default=256 * 1024)
    parser.add_argument("--socks-port", type=int, default=1080)
    args = parser.parse_args()
    if not 1280 <= args.max_ip_length < 1452:
        parser.error("--max-ip-length must be in 1280..1451")
    report = run(args)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
