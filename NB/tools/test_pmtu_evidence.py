#!/usr/bin/env python3
from pmtu_evidence import POLICY, build_report


def valid_input() -> dict:
    return {"metadata": {"commit": "abc", "deployment": "dep", "profile": "kz"}, "cases": [
        {"scenario": family, "evidence": {"address_family": family,
            "df_or_ptb_observed": True, "ip_mtu": 1452, "quic_ip_mtu": 1428,
            "payload_integrity": "ok", "session_continued": True}}
        for family in ("ipv4", "ipv6")
    ] + [
        {"scenario": "blackhole-fallback", "evidence": {"fault_injected": True,
            "mtu_before": 1452, "mtu_after": 1280, "payload_integrity": "ok",
            "session_continued": True, "recovery_ms": 900}},
        {"scenario": "udp-fragmentation", "evidence": {"max_payload_integrity": "ok",
            "out_of_order": "ok", "duplicate": "ignored", "timeout": "expired",
            "conflict": "rejected", "socks_ipv4": "ok", "socks_ipv6": "ok",
            "socks_domain": "ok"}},
    ]}


def main() -> None:
    report = build_report(valid_input())
    assert report["status"] == "passed" and report["policy"] == POLICY
    broken = valid_input()
    broken["cases"][2]["evidence"]["session_continued"] = False
    report = build_report(broken)
    assert report["status"] == "failed"
    assert not report["cases"][2]["checks"]["session_continued"]
    missing = valid_input()
    del missing["metadata"]["deployment"]
    assert build_report(missing)["status"] == "failed"
    print("pmtu_evidence tests passed")


if __name__ == "__main__":
    main()
