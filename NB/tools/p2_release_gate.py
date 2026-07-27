#!/usr/bin/env python3
"""Combine P2-A/B/C evidence into a strict or signed restricted decision."""
from __future__ import annotations

import argparse
import json
import pathlib

import p2_release_waiver


def strict_failures(pmtu: dict, fec: dict, fault: dict, soak: dict) -> list[str]:
    failures = []
    pmtu_cases = {case.get("scenario") for case in pmtu.get("cases", []) if case.get("passed")}
    required_pmtu = {"ipv4", "ipv6", "blackhole-fallback", "udp-fragmentation"}
    pmtu_checks_valid = all(case.get("passed") and case.get("checks") and
        all(case["checks"].values()) for case in pmtu.get("cases", [])
        if case.get("scenario") in required_pmtu)
    if (pmtu.get("schema_version") != 1 or pmtu.get("policy") != "p2-a-pmtu-v1" or
        pmtu.get("status") != "passed" or not required_pmtu <= pmtu_cases or
        not pmtu_checks_valid or not all(pmtu.get("metadata_checks", {}).values())):
        failures.append("P2-A-pmtu")
    if fec.get("status") != "admit" or fec.get("reasons"):
        failures.append("P2-B-fec-canary")
    fault_roles = {case.get("role") for case in fault.get("cases", []) if case.get("passed")}
    if fault.get("status") != "passed" or fault_roles != {"entry", "middle", "exit"}:
        failures.append("P2-C-fault-matrix")
    requested = float(soak.get("requested_duration_s", 0) or 0)
    validated = float(soak.get("validated_duration_s", 0) or 0)
    if soak.get("status") != "passed" or requested < 86400 or validated < requested:
        failures.append("P2-C-24h-soak")
    return failures


def restricted_valid(pmtu: dict, fec: dict, soak: dict, waiver: dict) -> bool:
    cases = {case.get("scenario"): case for case in pmtu.get("cases", [])}
    required = {"ipv4", "blackhole-fallback", "udp-fragmentation"}
    pmtu_ok = (pmtu.get("policy") == "p2-a-pmtu-restricted-v1" and
               pmtu.get("status") == "restricted-passed" and
               set(pmtu.get("deferred", [])) == {"ipv6"} and
               required <= set(cases) and all(cases[name].get("passed") and
               all(cases[name].get("checks", {}).values()) for name in required))
    fec_ok = (fec.get("policy") == "p2-b-fec-disposition-v1" and
              fec.get("status") == "rejected-for-active" and
              fec.get("production_mode") == "observe-only" and
              fec.get("active_fec_admitted") is False and bool(fec.get("reasons")))
    soak_ok = (soak.get("policy") == "p2-c-soak-disposition-v1" and
               soak.get("status") == "waiver-eligible" and
               float(soak.get("requested_duration_s", 0)) >= 86400 and
               float(soak.get("validated_duration_s", 0)) >= 43200 and
               int(soak.get("passed_chunks", 0)) >= 144 and
               int(soak.get("failed_chunks", 0)) == 1 and
               int(soak.get("payload_integrity_failures", 1)) == 0 and
               int(soak.get("injected_chunks", 1)) == 0)
    waiver_ok = (waiver.get("policy") == p2_release_waiver.POLICY and
                 waiver.get("state") == "approved" and
                 waiver.get("exceptions") == p2_release_waiver.EXCEPTIONS and
                 waiver.get("restrictions") == p2_release_waiver.RESTRICTIONS)
    return pmtu_ok and fec_ok and soak_ok and waiver_ok


def evaluate(pmtu: dict, fec: dict, fault: dict, soak: dict,
             waiver: dict | None = None) -> dict:
    failures = strict_failures(pmtu, fec, fault, soak)
    if not failures:
        return {"status": "passed", "failures": [], "policy": "p2-abc-v2",
                "release_scope": "general", "restrictions": {}}
    # Fault recovery is never waivable.
    if "P2-C-fault-matrix" in failures or waiver is None or not restricted_valid(
            pmtu, fec, soak, waiver):
        return {"status": "blocked", "failures": failures, "policy": "p2-abc-v2"}
    return {"status": "passed", "failures": [], "policy": "p2-abc-v2",
            "release_scope": "restricted", "restrictions": waiver["restrictions"],
            "waived": waiver["exceptions"], "approval_id": waiver["approval_id"]}


def main() -> int:
    parser = argparse.ArgumentParser(description="NB P2-A/B/C evidence gate")
    for name in ("pmtu", "fec", "fault", "soak"):
        parser.add_argument(f"--{name}", type=pathlib.Path, required=True)
    parser.add_argument("--waiver", type=pathlib.Path)
    parser.add_argument("--waiver-key-file", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    paths = {name: getattr(args, name) for name in ("pmtu", "fec", "fault", "soak")}
    reports = {name: json.loads(path.read_text(encoding="utf-8"))
               for name, path in paths.items()}
    waiver = None
    if args.waiver:
        waiver = json.loads(args.waiver.read_text(encoding="utf-8"))
        p2_release_waiver.verify(waiver, paths,
                                 p2_release_waiver.load_key(args.waiver_key_file))
    decision = evaluate(**reports, waiver=waiver)
    encoded = json.dumps(decision, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if decision["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
