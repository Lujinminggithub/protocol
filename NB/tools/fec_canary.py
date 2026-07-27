#!/usr/bin/env python3
"""Fail-closed decision for paired FEC canary measurements."""
from __future__ import annotations

import argparse
import json
import pathlib


def evaluate(report: dict) -> dict:
    reasons: list[str] = []
    pairs = report.get("pairs")
    if report.get("schema_version") != 1 or not isinstance(pairs, list):
        return {"status": "reject", "reasons": ["invalid-schema"]}
    if len(pairs) < 3:
        reasons.append("insufficient-paired-rounds")
    improvements = []
    overheads = []
    recoveries = 0
    for pair in pairs:
        baseline, candidate = pair.get("baseline", {}), pair.get("candidate", {})
        if baseline.get("integrity") != "ok" or candidate.get("integrity") != "ok":
            reasons.append("payload-integrity")
        if not candidate.get("fec_active"):
            reasons.append("candidate-fec-not-active")
        base_latency = float(baseline.get("latency_p95_ms", 0) or 0)
        fec_latency = float(candidate.get("latency_p95_ms", 0) or 0)
        base_bytes = float(baseline.get("wire_bytes", 0) or 0)
        fec_bytes = float(candidate.get("wire_bytes", 0) or 0)
        if min(base_latency, fec_latency, base_bytes, fec_bytes) <= 0:
            reasons.append("missing-measurement")
            continue
        improvements.append(100.0 * (base_latency - fec_latency) / base_latency)
        overheads.append(100.0 * (fec_bytes - base_bytes) / base_bytes)
        recoveries += int(candidate.get("recovered_packets", 0) or 0)
        if float(candidate.get("cpu_pct", 0) or 0) - float(baseline.get("cpu_pct", 0) or 0) > 15:
            reasons.append("cpu-regression")
        if float(candidate.get("memory_mb", 0) or 0) - float(baseline.get("memory_mb", 0) or 0) > 64:
            reasons.append("memory-regression")
    if overheads and max(overheads) > 35:
        reasons.append("bandwidth-overhead")
    mean_improvement = sum(improvements) / len(improvements) if improvements else 0.0
    if improvements and (mean_improvement < 5 or min(improvements) < -5):
        reasons.append("latency-benefit")
    if recoveries <= 0:
        reasons.append("no-recovery-observed")
    return {"status": "admit" if not reasons else "reject",
            "reasons": sorted(set(reasons)), "paired_rounds": len(pairs),
            "latency_improvement_mean_pct": round(mean_improvement, 3),
            "bandwidth_overhead_max_pct": round(max(overheads, default=0.0), 3),
            "recovered_packets": recoveries}


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate paired baseline/FEC canary evidence")
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    decision = evaluate(json.loads(args.report.read_text(encoding="utf-8")))
    encoded = json.dumps(decision, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if decision["status"] == "admit" else 1


if __name__ == "__main__":
    raise SystemExit(main())
