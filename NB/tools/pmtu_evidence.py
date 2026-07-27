#!/usr/bin/env python3
"""Build a fail-closed P2-A report from captured PMTU and UDP evidence."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib


POLICY = "p2-a-pmtu-v1"
REQUIRED = ("ipv4", "ipv6", "blackhole-fallback", "udp-fragmentation")


def _mtu_case(case: dict, family: str) -> dict:
    evidence = case.get("evidence", {})
    checks = {
        "address_family": evidence.get("address_family") == family,
        "df_or_ptb_observed": evidence.get("df_or_ptb_observed") is True,
        "ip_mtu_valid": int(evidence.get("ip_mtu", 0) or 0) >= 1280,
        "quic_mtu_valid": int(evidence.get("quic_ip_mtu", 0) or 0) >= 1280,
        "payload_integrity": evidence.get("payload_integrity") == "ok",
        "session_continued": evidence.get("session_continued") is True,
    }
    return {**case, "checks": checks, "passed": all(checks.values())}


def _blackhole_case(case: dict) -> dict:
    evidence = case.get("evidence", {})
    before = int(evidence.get("mtu_before", 0) or 0)
    after = int(evidence.get("mtu_after", 0) or 0)
    checks = {
        "fault_injected": evidence.get("fault_injected") is True,
        "actual_mtu_reduction": before > after >= 1200,
        "payload_integrity": evidence.get("payload_integrity") == "ok",
        "session_continued": evidence.get("session_continued") is True,
        "recovery_measured": float(evidence.get("recovery_ms", 0) or 0) > 0,
    }
    return {**case, "checks": checks, "passed": all(checks.values())}


def _udp_case(case: dict) -> dict:
    evidence = case.get("evidence", {})
    checks = {
        "max_payload_integrity": evidence.get("max_payload_integrity") == "ok",
        "out_of_order": evidence.get("out_of_order") == "ok",
        "duplicate": evidence.get("duplicate") == "ignored",
        "timeout": evidence.get("timeout") == "expired",
        "conflict": evidence.get("conflict") == "rejected",
        "socks_ipv4": evidence.get("socks_ipv4") == "ok",
        "socks_ipv6": evidence.get("socks_ipv6") == "ok",
        "socks_domain": evidence.get("socks_domain") == "ok",
    }
    return {**case, "checks": checks, "passed": all(checks.values())}


def build_report(raw: dict) -> dict:
    source = raw.get("cases", [])
    by_name = {case.get("scenario"): case for case in source if isinstance(case, dict)}
    cases = []
    for name in REQUIRED:
        case = {**by_name.get(name, {}), "scenario": name}
        if name == "ipv4":
            evaluated = _mtu_case(case, "ipv4")
        elif name == "ipv6":
            evaluated = _mtu_case(case, "ipv6")
        elif name == "blackhole-fallback":
            evaluated = _blackhole_case(case)
        else:
            evaluated = _udp_case(case)
        cases.append(evaluated)
    metadata = raw.get("metadata", {})
    metadata_checks = {
        "commit": bool(metadata.get("commit")),
        "deployment": bool(metadata.get("deployment")),
        "profile": bool(metadata.get("profile")),
    }
    return {
        "schema_version": 1,
        "policy": POLICY,
        "status": "passed" if all(case["passed"] for case in cases)
            and all(metadata_checks.values()) else "failed",
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "metadata": metadata,
        "metadata_checks": metadata_checks,
        "cases": cases,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Build validated P2-A PMTU evidence")
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    report = build_report(json.loads(args.input.read_text(encoding="utf-8")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
