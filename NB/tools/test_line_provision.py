#!/usr/bin/env python3
import copy
import pathlib
import tempfile

from line_provision import (assert_qualified, build_artifacts, conservative_candidate,
                            probe_cache_key, qualification_rate, transient_probe_failure,
                            validate_line)


def recommendation(cc: str) -> dict:
    return {
        "confidence": "load-qualified",
        "auto_apply_allowed": False,
        "cc": cc,
        "cwin_max_bytes": 786432 if cc == "cubic" else None,
        "reorder_gap": 160,
        "reorder_delay_us": 500000,
        "mtu_max": 1452,
        "mtu_evidence": {"confidence": "quic-and-df"},
    }


def candidate(package: float = 10, ratio: float = 1.25) -> dict:
    segment = {"quic": {"packets_observed": 12000, "windows_valid": 6}}
    entry = copy.deepcopy(segment); entry["candidate"] = recommendation("cubic")
    middle = copy.deepcopy(segment); middle["candidate"] = recommendation("bbr")
    return {
        "service_package": {"committed_mbps": package,
                            "upstream_mbps": package,"downstream_mbps": package,
                            "qualification_mbps": package * ratio,
                            "headroom_ratio": ratio},
        "admission": {"status": "admitted", "achieved_mbps": package * ratio * 0.96},
        "segments": {"entry_middle": entry, "middle_exit": middle},
    }


def main() -> None:
    assert transient_probe_failure("ConnectionAbortedError: [WinError 10053]")
    assert transient_probe_failure("TimeoutError: timed out")
    assert not transient_probe_failure("容量准入失败: insufficient-throughput")
    assert qualification_rate(5, 1.25) == 6.25
    assert qualification_rate(10, 1.25) == 12.5
    assert qualification_rate(15, 1.25) == 18.75
    line = {
        "line_id": "line-10m-01",
        "hosts_file": "hosts.json",
        "baseline_profile": "profile.json",
        "package_mbps": 10,
        "upstream_mbps": 8,
        "downstream_mbps": 12,
        "client": {"name": "测试线路", "port": 1023, "username": "u@ser",
                   "password_env": "CLIENT_PASSWORD"},
    }
    validate_line(line)
    hosts = {
        "entry": {"name": "entry", "host": "192.0.2.10"},
        "transport": {"entry": {"udp_gso": False}, "middle": {"udp_gso": False}},
        "exits": [{"name": "exit-primary", "host": "198.51.100.20", "port": 4443,
                   "weight": 1, "capacity": 100, "fixed_exit": "exit"}],
    }
    baseline = {
        "schema_version": 3, "line_id": "old", "fixed_exit": "exit",
        "transport": {"entry_middle": {}, "middle_exit": {}},
    }
    artifacts = build_artifacts(line, candidate(), hosts, baseline, "p:a/ss", 1.25)
    assert artifacts["profile"]["status"] == "stable-qualified"
    assert artifacts["profile"]["schema_version"] == 4
    assert "cwin_max_bytes" not in artifacts["hosts"]["transport"]["entry"]
    assert "cwin_max_bytes" not in artifacts["hosts"]["transport"]["middle"]
    assert artifacts["policy"]["tenants"][0]["rate_up_kbps"] == 8000
    assert artifacts["policy"]["tenants"][0]["rate_down_kbps"] == 12000
    assert artifacts["policy"]["schema_version"] == 3
    assert artifacts["policy"]["tenants"][0]["burst_up_bytes"] == 1000000
    assert artifacts["policy"]["tenants"][0]["burst_down_bytes"] == 1500000
    assert artifacts["client"]["server"] == "192.0.2.10"
    assert "u%40ser:p%3Aa%2Fss@" in artifacts["client"]["shadowrocket_url"]

    approved = candidate()
    for segment in approved["segments"].values():
        segment["candidate"]["auto_apply_allowed"] = True
    applied = build_artifacts(line, approved, hosts, baseline, "p:a/ss", 1.25)
    assert applied["hosts"]["transport"]["entry"]["cwin_max_bytes"] == 786432
    assert "cwin_max_bytes" not in applied["hosts"]["transport"]["middle"]

    failed = candidate(); failed["admission"] = {"status": "rejected", "reasons": ["insufficient-throughput"]}
    try:
        assert_qualified(failed, 10, 1.25)
        raise AssertionError("容量不足必须拒绝")
    except ValueError as error:
        assert "容量准入失败" in str(error)

    marginal = candidate(); marginal["admission"]["achieved_mbps"] = 8.9
    try:
        assert_qualified(marginal, 10, 1.25)
        raise AssertionError("业务整形未达到平均值 90% 不得生成稳定配置")
    except ValueError as error:
        assert "稳定配置余量不足" in str(error)

    weak = candidate(); weak["segments"]["middle_exit"]["quic"]["windows_valid"] = 5
    try:
        assert_qualified(weak, 10, 1.25)
        raise AssertionError("样本不足必须拒绝")
    except ValueError as error:
        assert "最小有效窗口" in str(error)

    provisional = build_artifacts(
        line, weak, hosts, baseline, "p:a/ss", 1.25,
        allow_conservative_fallback=True)
    assert provisional["qualification"]["status"] == "provisional"
    assert provisional["profile"]["status"] == "provisional-conservative"
    assert provisional["profile"]["service_package"]["admission"] == "pending-validation"
    assert provisional["hosts"]["transport"] == hosts["transport"]
    assert provisional["client"]["shadowrocket_url"].startswith("socks5://")

    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        hosts_path, profile_path = root / "hosts.json", root / "profile.json"
        hosts_path.write_text("{}\n", encoding="utf-8")
        profile_path.write_text("{}\n", encoding="utf-8")
        fallback = conservative_candidate(hosts_path, profile_path, 10, 1.25,
                                          "probe unavailable")
        assert fallback["admission"]["status"] == "pending-validation"
        assert fallback["fallback_reason"] == "probe unavailable"
        key_a = probe_cache_key(hosts_path, profile_path, 10, 8, 12, 1.25, 90, 30)
        key_b = probe_cache_key(hosts_path, profile_path, 10, 8, 12, 1.25, 90, 30)
        key_c = probe_cache_key(hosts_path, profile_path, 10, 8, 12, 1.25, 120, 30)
        assert key_a == key_b and key_a != key_c
    print("line_provision tests passed")


if __name__ == "__main__":
    main()
