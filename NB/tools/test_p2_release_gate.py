#!/usr/bin/env python3
import copy
import datetime as dt
import json
import pathlib
import tempfile

from p2_release_gate import evaluate
from p2_release_waiver import create, verify
from test_pmtu_evidence import valid_input
from pmtu_evidence import build_report


def strict_inputs() -> tuple[dict, dict, dict, dict]:
    pmtu = build_report(valid_input())
    fec = {"status": "admit", "reasons": []}
    fault = {"status": "passed", "cases": [
        {"role": role, "passed": True} for role in ("entry", "middle", "exit")]}
    soak = {"status": "passed", "requested_duration_s": 86400,
            "validated_duration_s": 86400}
    return pmtu, fec, fault, soak


def restricted_inputs() -> tuple[dict, dict, dict, dict]:
    pmtu = {"schema_version": 1, "policy": "p2-a-pmtu-restricted-v1",
            "status": "restricted-passed", "deferred": ["ipv6"], "cases": [
        {"scenario": name, "passed": True, "checks": {"evidence": True}}
        for name in ("ipv4", "blackhole-fallback", "udp-fragmentation")]}
    fec = {"schema_version": 1, "policy": "p2-b-fec-disposition-v1",
           "status": "rejected-for-active", "production_mode": "observe-only",
           "active_fec_admitted": False, "reasons": ["quantitative rejection"]}
    fault = {"status": "passed", "cases": [
        {"role": role, "passed": True} for role in ("entry", "middle", "exit")]}
    soak = {"schema_version": 1, "policy": "p2-c-soak-disposition-v1",
            "status": "waiver-eligible", "requested_duration_s": 86400,
            "validated_duration_s": 45930, "passed_chunks": 153,
            "failed_chunks": 1, "payload_integrity_failures": 0,
            "injected_chunks": 0}
    return pmtu, fec, fault, soak


def main() -> None:
    pmtu, fec, fault, soak = strict_inputs()
    assert evaluate(pmtu, fec, fault, soak)["release_scope"] == "general"
    soak["validated_duration_s"] = 86399
    assert evaluate(pmtu, fec, fault, soak)["failures"] == ["P2-C-24h-soak"]

    pmtu, fec, fault, soak = restricted_inputs()
    now = dt.datetime(2026, 7, 27, tzinfo=dt.timezone.utc)
    key = b"p2-release-test-key-material-32-bytes-minimum"
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        values = {"pmtu": pmtu, "fec": fec, "fault": fault, "soak": soak}
        paths = {}
        for name, value in values.items():
            path = root / f"{name}.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            paths[name] = path
        waiver = create(paths, "test-approver", "deployment-1",
                        "2026-08-03T00:00:00Z", "test decision", key, now)
        verify(waiver, paths, key, now)
        decision = evaluate(pmtu, fec, fault, soak, waiver)
        assert decision["status"] == "passed"
        assert decision["release_scope"] == "restricted"

        tampered = copy.deepcopy(waiver)
        tampered["restrictions"]["fec_active"] = True
        try:
            verify(tampered, paths, key, now)
            raise AssertionError("tampered waiver accepted")
        except ValueError:
            pass
        try:
            verify(waiver, paths, key, dt.datetime(2026, 8, 4, tzinfo=dt.timezone.utc))
            raise AssertionError("expired waiver accepted")
        except ValueError:
            pass
        paths["fec"].write_text("{}", encoding="utf-8")
        try:
            verify(waiver, paths, key, now)
            raise AssertionError("drifted evidence accepted")
        except ValueError:
            pass

    fault["cases"][0]["passed"] = False
    assert evaluate(pmtu, fec, fault, soak, waiver)["status"] == "blocked"
    soak["payload_integrity_failures"] = 1
    assert evaluate(pmtu, fec, {"status": "passed", "cases": [
        {"role": role, "passed": True} for role in ("entry", "middle", "exit")]},
        soak, waiver)["status"] == "blocked"
    print("p2_release_gate tests passed")


if __name__ == "__main__":
    main()
