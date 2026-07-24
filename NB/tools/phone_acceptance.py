#!/usr/bin/env python3
"""手机直播人工验收期间的三端被动观测与时间对齐。"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import time

import deploy
import line_probe
import nb_observe
from netem_matrix import atomic_report, summarize_observation, validate_cluster


BEIJING = dt.timezone(dt.timedelta(hours=8))


def timestamps() -> dict:
    now = dt.datetime.now(dt.timezone.utc)
    return {
        "utc": now.isoformat().replace("+00:00", "Z"),
        "beijing": now.astimezone(BEIJING).isoformat(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="NB KZ 下午手机直播被动验收")
    parser.add_argument("--duration", type=int, default=30 * 60)
    parser.add_argument("--interval", type=int, default=10)
    parser.add_argument("--label", default="KZ-afternoon-phone-live")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.duration < 60 or args.duration > 4 * 3600:
        parser.error("--duration 必须在 60 秒到 4 小时之间")
    if args.interval < 5 or args.interval > 60:
        parser.error("--interval 必须在 5..60 秒")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output or deploy.BUILD_DIR / "phone" / f"phone-{stamp}.json"
    log_dir = output.parent / f"{output.stem}-logs"
    report = {
        "schema_version": 1,
        "label": args.label,
        "started_at": timestamps(),
        "requested_duration_s": args.duration,
        "mode": "passive-no-synthetic-load",
        "samples": [],
        "status": "running",
    }
    offsets = {}
    state = None
    try:
        baseline = nb_observe.collect()
        validate_cluster(baseline)
        report["baseline"] = summarize_observation([baseline])
        offsets = {role: line_probe.log_offset(role) for role in ("entry", "middle", "exit")}
        deadline = time.monotonic() + args.duration
        while time.monotonic() < deadline:
            snapshot = nb_observe.collect()
            alerts, state = nb_observe.evaluate(snapshot, state)
            report["samples"].append({
                "at": timestamps(),
                "snapshot": snapshot,
                "alerts": alerts,
            })
            report["elapsed_s"] = args.duration - max(0, deadline - time.monotonic())
            atomic_report(output, report)
            time.sleep(min(args.interval, max(0, deadline - time.monotonic())))
        report["status"] = "completed"
    except KeyboardInterrupt:
        report["status"] = "stopped"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        log_dir.mkdir(parents=True, exist_ok=True)
        log_errors = []
        for role, offset in offsets.items():
            try:
                (log_dir / f"{role}.log").write_text(
                    line_probe.log_since(role, offset), encoding="utf-8")
            except Exception as error:
                log_errors.append({"role": role, "error": f"{type(error).__name__}: {error}"})
        report["log_dir"] = str(log_dir)
        report["log_errors"] = log_errors
        report["finished_at"] = timestamps()
        snapshots = [item["snapshot"] for item in report["samples"]]
        report["summary"] = summarize_observation(snapshots)
        report["alert_windows"] = sum(bool(item["alerts"]) for item in report["samples"])
        report["critical_windows"] = sum(any(alert["severity"] == "critical" for alert in item["alerts"])
                                         for item in report["samples"])
        atomic_report(output, report)
    print(f"手机验收报告: {output}")
    return 0 if report["status"] in ("completed", "stopped") and not report["critical_windows"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
