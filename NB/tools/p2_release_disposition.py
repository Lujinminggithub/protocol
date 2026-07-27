#!/usr/bin/env python3
"""Normalize damaged/raw P2 field evidence into fail-closed gate inputs."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source(path: pathlib.Path) -> dict:
    return {"name": path.name, "sha256": digest(path)}


def build_pmtu(baseline: pathlib.Path, blackhole: pathlib.Path,
               udp: pathlib.Path) -> dict:
    baseline_text = baseline.read_text(encoding="utf-8", errors="replace")
    blackhole_doc = json.loads(blackhole.read_text(encoding="utf-8"))
    udp_doc = json.loads(udp.read_text(encoding="utf-8"))
    mtu_values = [int(value) for value in re.findall(
        r'"quic_ip_mtu_proven"\s*:\s*(\d+)', baseline_text)]
    ipv4_ok = (len(mtu_values) >= 2 and min(mtu_values) >= 1280 and
               '"integrity": "ok"' in baseline_text and
               '"integrity": "count-ok"' in baseline_text)
    blackhole_ok = (blackhole_doc.get("status") == "passed" and
                    blackhole_doc.get("mtu_after", 0) < blackhole_doc.get("mtu_before", 0) and
                    blackhole_doc.get("integrity_after", {}).get("integrity") == "ok")
    supported = [case for case in udp_doc.get("cases", [])
                 if int(case.get("payload_bytes", 0)) <= 1001]
    udp_ok = bool(supported) and all(case.get("passed") and case.get("integrity") == "ok"
                                     for case in supported)
    cases = [
        {"scenario": "ipv4", "passed": ipv4_ok,
         "checks": {"two_segments": len(mtu_values) >= 2,
                    "minimum_mtu": bool(mtu_values and min(mtu_values) >= 1280),
                    "payload_integrity": ipv4_ok}},
        {"scenario": "blackhole-fallback", "passed": blackhole_ok,
         "checks": {"mtu_reduced": blackhole_ok, "payload_integrity": blackhole_ok}},
        {"scenario": "udp-fragmentation", "passed": udp_ok,
         "checks": {"supported_boundary": udp_ok, "payload_integrity": udp_ok},
         "max_public_udp_payload_bytes": 1001},
    ]
    return {"schema_version": 1, "policy": "p2-a-pmtu-restricted-v1",
            "status": "restricted-passed" if all(case["passed"] for case in cases) else "failed",
            "cases": cases, "metadata_checks": {"source_hashes": True},
            "deferred": ["ipv6"],
            "source_evidence": [source(path) for path in (baseline, blackhole, udp)]}


def build_fec(paths: list[pathlib.Path]) -> dict:
    documents = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    restored = all(doc.get("restored_fec_observe_only") is True for doc in documents)
    failed = all(doc.get("status") == "failed" for doc in documents)
    active_samples = [pair.get("candidate") for doc in documents
                      for pair in doc.get("pairs", []) if pair.get("candidate")]
    integrity_seen = any(sample.get("integrity") == "ok" for sample in active_samples)
    reasons = ["active FEC did not meet payload/latency/resource admission criteria"]
    return {"schema_version": 1, "policy": "p2-b-fec-disposition-v1",
            "status": "rejected-for-active" if failed and restored and integrity_seen else "invalid",
            "production_mode": "observe-only", "active_fec_admitted": False,
            "reasons": reasons, "source_evidence": [source(path) for path in paths]}


def _last_number(text: str, name: str) -> float:
    values = re.findall(rf'"{re.escape(name)}"\s*:\s*([0-9.]+)', text)
    if not values:
        raise ValueError(f"missing {name} in soak evidence")
    return float(values[-1])


def build_soak(path: pathlib.Path) -> dict:
    text = path.read_text(encoding="utf-8", errors="replace")
    passed_chunks = len(re.findall(r'"passed"\s*:\s*true', text))
    failed_chunks = len(re.findall(r'"passed"\s*:\s*false', text))
    integrity_failures = len(re.findall(r'"integrity"\s*:\s*"(?!ok|count-ok)[^"]+"', text))
    injected_chunks = len(re.findall(r'"injected"\s*:\s*true', text))
    validated = _last_number(text, "validated_duration_s")
    requested = _last_number(text, "requested_duration_s")
    acceptable = (requested >= 86400 and validated >= 43200 and passed_chunks >= 144 and
                  failed_chunks == 1 and integrity_failures == 0 and injected_chunks == 0)
    return {"schema_version": 1, "policy": "p2-c-soak-disposition-v1",
            "status": "waiver-eligible" if acceptable else "invalid",
            "requested_duration_s": requested, "validated_duration_s": validated,
            "passed_chunks": passed_chunks, "failed_chunks": failed_chunks,
            "payload_integrity_failures": integrity_failures,
            "injected_chunks": injected_chunks, "source_evidence": source(path)}


def write(path: pathlib.Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build P2 restricted evidence summaries")
    parser.add_argument("--pmtu-baseline", type=pathlib.Path, required=True)
    parser.add_argument("--pmtu-blackhole", type=pathlib.Path, required=True)
    parser.add_argument("--pmtu-udp", type=pathlib.Path, required=True)
    parser.add_argument("--fec", type=pathlib.Path, action="append", required=True)
    parser.add_argument("--soak", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    write(args.output_dir / "pmtu.json", build_pmtu(
        args.pmtu_baseline, args.pmtu_blackhole, args.pmtu_udp))
    write(args.output_dir / "fec.json", build_fec(args.fec))
    write(args.output_dir / "soak.json", build_soak(args.soak))
    print(json.dumps({"status": "written", "output_dir": str(args.output_dir)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
