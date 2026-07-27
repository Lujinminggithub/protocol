#!/usr/bin/env python3
"""Run paired baseline/FEC measurements under selective middle-exit netem."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import re
import time

import deploy
import fec_canary
import line_probe
import nb_observe
import v15_fec_test
from netem_matrix import SelectiveNetem


SENT_RE = re.compile(r"\bSent\s+(\d+)\s+bytes\b")


def percentile(values: list[float], ratio: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, max(0, int((len(ordered) - 1) * ratio + 0.5)))
    return ordered[index]


def qdisc_bytes(stats: str) -> int:
    values = [int(value) for value in SENT_RE.findall(stats)]
    if not values:
        raise RuntimeError("netem qdisc did not expose a byte counter")
    return max(values)


def process_resources() -> dict:
    samples = []
    for role in ("middle", "exit"):
        connection = deploy.connect(role)
        try:
            raw = deploy.run(connection,
                "printf 'hz='; getconf CLK_TCK; for p in $(pgrep -x nb_node); do "
                "awk '{print $14+$15}' /proc/$p/stat; awk '/VmRSS:/ {print $2}' /proc/$p/status; done")
        finally:
            connection.close()
        hz_match = re.search(r"^hz=(\d+)$", raw, re.MULTILINE)
        values = [int(value) for value in re.findall(r"^\d+$", raw, re.MULTILINE)]
        if not hz_match or not values or len(values) % 2:
            raise RuntimeError(f"unable to collect {role} process resources")
        samples.append((sum(values[0::2]), sum(values[1::2]), int(hz_match.group(1))))
    if len({item[2] for item in samples}) != 1:
        raise RuntimeError("middle/exit clock tick rates differ")
    return {"cpu_ticks": sum(item[0] for item in samples),
            "memory_mb": sum(item[1] for item in samples) / 1024.0,
            "clock_hz": samples[0][2]}


def fec_snapshot() -> dict:
    snapshot = nb_observe.collect()
    if snapshot.get("collection_errors"):
        raise RuntimeError("cluster metrics collection failed")
    relevant = [worker for worker in snapshot.get("workers", [])
                if worker.get("role") in ("middle", "exit")]
    if {worker.get("role") for worker in relevant} != {"middle", "exit"}:
        raise RuntimeError("middle/exit metrics are incomplete")
    if any(worker.get("health", {}).get("status") != "ok" for worker in relevant):
        raise RuntimeError("middle/exit has an unhealthy worker")
    deployments = {worker.get("health", {}).get("release_id") for worker in relevant}
    profiles = {(worker.get("health", {}).get("line_profile"),
                 worker.get("health", {}).get("line_profile_schema")) for worker in relevant}
    if len(deployments) != 1 or len(profiles) != 1:
        raise RuntimeError("deployment/profile drift during FEC canary")
    return {
        "active": all(int(worker.get("metrics", {}).get("fec", {}).get("active", 0)) == 1
                      for worker in relevant),
        "recovered": sum(int(worker.get("metrics", {}).get("fec", {}).get("recovered", 0) or 0)
                         for worker in relevant),
        "deployment": next(iter(deployments)),
        "profile": list(next(iter(profiles))),
    }


def atomic_report(path: pathlib.Path, report: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + f".{os.getpid()}.tmp")
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def wait_phase_ready(candidate: bool, timeout_s: int = 90) -> dict:
    deadline = time.monotonic() + timeout_s
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            snapshot = fec_snapshot()
            if bool(snapshot["active"]) != candidate:
                raise RuntimeError("FEC stage has not converged")
            line_probe.run_integrity_probe(deploy._role_host("entry")["host"],
                                           v15_fec_test.SOCKS_PORT, 64 * 1024, 15)
            return snapshot
        except Exception as error:
            last_error = error
            time.sleep(3)
    raise RuntimeError(f"phase readiness timed out: {type(last_error).__name__ if last_error else 'unknown'}")


def run_phase(injector: SelectiveNetem, expected_sha: str, downloads: int,
              loss_pct: float, delay_ms: int, jitter_ms: int, candidate: bool,
              candidate_profile: str) -> dict:
    stage = "canary-robust" if candidate and candidate_profile == "robust" else "canary"
    v15_fec_test.apply_stage_env(stage if candidate else "auto")
    time.sleep(5)
    before = wait_phase_ready(candidate)
    if candidate and not before["active"]:
        raise RuntimeError("candidate stage did not enable FEC on middle/exit")
    if not candidate and before["active"]:
        raise RuntimeError("baseline stage unexpectedly has active FEC")
    injector.configure(loss_pct, delay_ms, jitter_ms, 0.0)
    wire_before = qdisc_bytes(injector.stats())
    resources_before = process_resources()
    started = time.monotonic()
    times = v15_fec_test.curl_payload(downloads, expected_sha, max_time=60)
    resources_after = process_resources()
    after = fec_snapshot()
    elapsed = time.monotonic() - started
    cpu_pct = max(0, resources_after["cpu_ticks"] - resources_before["cpu_ticks"]) / resources_after["clock_hz"] / elapsed * 100.0
    return {
        "integrity": "ok",
        "fec_active": bool(after["active"]),
        "latency_p95_ms": percentile(times, 0.95) * 1000.0,
        "latency_samples_ms": [round(value * 1000.0, 3) for value in times],
        "wire_bytes": max(0, qdisc_bytes(injector.stats()) - wire_before),
        "cpu_pct": cpu_pct,
        "memory_mb": resources_after["memory_mb"],
        "recovered_packets": max(0, int(after["recovered"]) - int(before["recovered"])),
        "elapsed_s": round(elapsed, 3),
        "deployment": after["deployment"],
        "profile": after["profile"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Paired quantitative FEC canary with selective netem")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--downloads", type=int, default=4)
    parser.add_argument("--loss-pct", type=float, default=5.0)
    parser.add_argument("--delay-ms", type=int, default=20)
    parser.add_argument("--jitter-ms", type=int, default=5)
    parser.add_argument("--candidate-profile", choices=("balanced", "robust"), default="balanced")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--resume-from", type=pathlib.Path)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    if args.rounds < 3 or args.rounds > 10 or args.downloads < 3 or args.downloads > 20:
        parser.error("rounds must be 3..10 and downloads must be 3..20")
    if not 0 < args.loss_pct <= 15 or min(args.delay_ms, args.jitter_ms) < 0:
        parser.error("invalid impairment")

    middle = deploy._role_host("middle")
    exit_host = deploy._role_host("exit")
    injector = SelectiveNetem("middle", exit_host["host"],
        middle.get("private_ip") or middle["host"], port_direction="src")
    report = {
        "schema_version": 1,
        "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "segment": "exit-middle-download",
        "impairment": {"loss_pct": args.loss_pct, "delay_ms": args.delay_ms,
                       "jitter_ms": args.jitter_ms},
        "candidate_profile": args.candidate_profile,
        "pairs": [],
    }
    if args.resume_from:
        previous = json.loads(args.resume_from.read_text(encoding="utf-8"))
        if (previous.get("schema_version") != 1 or previous.get("impairment") != report["impairment"] or
                previous.get("candidate_profile", "balanced") != report["candidate_profile"]):
            parser.error("resume report schema, impairment, or candidate profile does not match")
        report["pairs"] = [pair for pair in previous.get("pairs", [])
                           if pair.get("baseline") and pair.get("candidate")]
        report["resumed_from"] = str(args.resume_from)
        if len(report["pairs"]) >= args.rounds:
            parser.error("resume report already has enough complete pairs")
    rc = 1
    try:
        report["preflight"] = injector.open()
        if not args.apply:
            report["status"] = "preflight-only"
            rc = 0
        else:
            injector.install()
            expected_sha = v15_fec_test.prepare_payload_target()
            for index in range(len(report["pairs"]) + 1, args.rounds + 1):
                pair = {"round": index}
                report["pairs"].append(pair)
                pair["baseline"] = run_phase(injector, expected_sha, args.downloads,
                    args.loss_pct, args.delay_ms, args.jitter_ms, False, args.candidate_profile)
                atomic_report(args.output, report)
                pair["candidate"] = run_phase(injector, expected_sha, args.downloads,
                    args.loss_pct, args.delay_ms, args.jitter_ms, True, args.candidate_profile)
                atomic_report(args.output, report)
            report["decision"] = fec_canary.evaluate(report)
            report["status"] = report["decision"]["status"]
            rc = 0 if report["status"] == "admit" else 1
    except Exception as error:
        report["status"] = "failed"
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        try:
            v15_fec_test.apply_stage_env("auto")
            report["restored_fec_observe_only"] = True
        except Exception as error:
            report["restored_fec_observe_only"] = False
            report["restore_error"] = f"{type(error).__name__}: {error}"
            rc = 1
        injector.close()
        report["finished_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
        atomic_report(args.output, report)
    print(json.dumps({"status": report["status"], "decision": report.get("decision"),
                      "output": str(args.output)}, ensure_ascii=False))
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
