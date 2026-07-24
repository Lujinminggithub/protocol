#!/usr/bin/env python3
"""NB 三跳持续负载验收，分段落盘并在失败时自动保全现场。"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import time
from types import SimpleNamespace

import deploy
import nb_diag
import nb_observe
from netem_matrix import run_case, validate_cluster


def atomic_write(path: pathlib.Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + f".{os.getpid()}.tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for attempt in range(5):
        try:
            os.replace(temporary, path)
            return
        except OSError:
            if attempt == 4: raise
            time.sleep(0.1 * (attempt + 1))


def apply_collection_tolerance(case: dict, streak: int, maximum: int) -> int:
    reasons = set(case.get("failure_reasons", []))
    if reasons == {"collection-error"} and case.get("load", {}).get("integrity") == "count-ok" and \
            case.get("integrity_before", {}).get("integrity") == "ok" and \
            case.get("integrity_after", {}).get("integrity") == "ok":
        streak += 1
        case["warnings"] = sorted(reasons)
        if streak < maximum:
            case["passed"] = True
            case["failure_reasons"] = []
            case["collection_error_tolerated"] = True
    elif "collection-error" not in reasons:
        streak = 0
    return streak


def main() -> int:
    parser = argparse.ArgumentParser(description="NB KZ 三跳 24 小时持续负载验收")
    parser.add_argument("--duration", type=int, default=24 * 3600)
    parser.add_argument("--chunk", type=int, default=300, help="单条持续 QUIC 负载时长")
    parser.add_argument("--interval", type=int, default=10)
    parser.add_argument("--target-mbps", type=float, default=4.0)
    parser.add_argument("--min-throughput-ratio", type=float, default=0.80)
    parser.add_argument("--max-queue-age-ms", type=int, default=1000)
    parser.add_argument("--integrity-bytes", type=int, default=256 * 1024)
    parser.add_argument("--socks-port", type=int, default=1080)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--max-consecutive-collection-errors", type=int, default=2)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    if args.duration < 20 or args.duration > 7 * 24 * 3600:
        parser.error("--duration 必须在 20 秒到 7 天之间")
    if args.max_consecutive_collection_errors < 1 or args.max_consecutive_collection_errors > 10:
        parser.error("--max-consecutive-collection-errors must be in 1..10")
    if args.chunk < 20 or args.chunk > 1800:
        parser.error("--chunk 必须在 20..1800 秒")

    started_wall = time.time()
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output or deploy.BUILD_DIR / "soak" / f"soak-{stamp}.json"
    report = {
        "schema_version": 2,
        "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "line": "-".join(deploy._role_host(role)["name"] for role in ("entry", "middle", "exit")),
        "fec_required_mode": "observe-only",
        "requested_duration_s": args.duration,
        "chunks": [],
        "status": "running",
    }
    if args.resume:
        if not output.exists(): parser.error("--resume requires an existing --output")
        report = json.loads(output.read_text(encoding="utf-8"))
        if report.get("status") in ("failed", "running"):
            report.setdefault("interruptions", []).append({
                "finished_at_utc": report.get("finished_at_utc") or
                    dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
                "fatal_error": report.get("fatal_error"),
                "incident_bundle": report.get("incident_bundle"),
                "elapsed_s": report.get("elapsed_s"),
                "reason": "failed" if report.get("status") == "failed" else
                    "operator-paused-for-phone-acceptance",
            })
        for key in ("finished_at_utc", "fatal_error", "incident_bundle", "bundle_error"):
            report.pop(key, None)
        report["status"] = "running"
        report["requested_duration_s"] = args.duration
        report["resumed_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    validated_duration = sum(float(c.get("load", {}).get("elapsed_s", 0) or 0)
                             for c in report.get("chunks", []) if c.get("passed"))
    collection_streak = int(report.get("collection_error_streak", 0) or 0)
    prior_elapsed = float(report.get("elapsed_s", 0) or 0)
    try:
        validate_cluster(nb_observe.collect())
        while validated_duration < args.duration:
            remaining = args.duration - int(validated_duration)
            case_args = SimpleNamespace(**vars(args))
            case_args.duration = min(args.chunk, max(20, remaining))
            case_args.delay_ms = 0
            case_args.jitter_ms = 0
            case_args.reorder_pct = 0.0
            case = run_case(case_args, 0.0, injected=False)
            collection_streak = apply_collection_tolerance(
                case, collection_streak, args.max_consecutive_collection_errors)
            case["chunk"] = len(report["chunks"]) + 1
            case["finished_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
            report["chunks"].append(case)
            if case["passed"]: validated_duration += float(case.get("load", {}).get("elapsed_s", 0) or 0)
            report["validated_duration_s"] = validated_duration
            report["collection_error_streak"] = collection_streak
            report["elapsed_s"] = prior_elapsed + time.time() - started_wall
            atomic_write(output, report)
            print(json.dumps({"chunk": case["chunk"], "passed": case["passed"],
                              "load": case.get("load"), "observation": case.get("observation")},
                             ensure_ascii=False))
            if not case["passed"]:
                report["status"] = "failed"
                directory, archive, _ = nb_diag.incident_bundle()
                report["incident_bundle"] = {"directory": str(directory), "archive": str(archive)}
                break
        else:
            report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["fatal_error"] = f"{type(error).__name__}: {error}"
        try:
            directory, archive, _ = nb_diag.incident_bundle()
            report["incident_bundle"] = {"directory": str(directory), "archive": str(archive)}
        except Exception as bundle_error:
            report["bundle_error"] = f"{type(bundle_error).__name__}: {bundle_error}"
    report["finished_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    report["elapsed_s"] = prior_elapsed + time.time() - started_wall
    atomic_write(output, report)
    print(f"soak 报告: {output}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
