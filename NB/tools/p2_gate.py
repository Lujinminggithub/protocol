#!/usr/bin/env python3
"""P2-A/B/C scope and Xray protocol-demo repository gate."""
from __future__ import annotations

import pathlib

from source_gate import enforce_source_line_limit


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> None:
    enforce_source_line_limit()
    required = (
        "src/nb_v2_metadata.c",
        "src/nb_v2_metadata.h",
        "src/nb_pmtu.c",
        "src/nb_pmtu.h",
        "tools/test_pmtu.c",
        "tools/pmtu_evidence.py",
        "tools/test_pmtu_evidence.py",
        "tools/pmtu_blackhole.py",
        "tools/udp_e2e_probe.py",
        "tools/fec_canary.py",
        "tools/fec_canary_run.py",
        "tools/test_fec_canary_run.py",
        "src/nb_fec_policy.c",
        "src/nb_fec_policy.h",
        "tools/test_fec_policy.c",
        "tools/v15_fec_test.py",
        "tools/test_fec_canary.py",
        "tools/p2_release_gate.py",
        "tools/test_p2_release_gate.py",
        "tools/p2_release_waiver.py",
        "tools/p2_release_disposition.py",
        "tools/test_p2_release_disposition.py",
        "tools/test_v2_metadata.c",
        "docs/18-P2-ABC实施与Xray协议Demo.md",
        "client/xraydemo/go.mod",
        "client/xraydemo/nbproto/metadata.go",
        "client/xraydemo/nbproto/metadata_test.go",
        "client/xraydemo/nbproto/xray_adapter.go",
        "client/xraydemo/nbproto/xray_adapter_test.go",
        "client/xraydemo/cmd/nbproto-demo/main.go",
    )
    for name in required:
        if not (ROOT / name).is_file():
            raise RuntimeError(f"missing P2 foundation file: {name}")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    roadmap = (ROOT / "docs/05-roadmap.md").read_text(encoding="utf-8")
    deploy = "\n".join((ROOT / name).read_text(encoding="utf-8")
        for name in ("tools/deploy.py", "tools/deploy_core.py"))
    for token in ("nb_v2_metadata_test", "tools/test_v2_metadata.c", "src/nb_v2_metadata.c",
                  "nb_pmtu_test", "tools/test_pmtu.c", "src/nb_pmtu.c",
                  "nb_fec_policy_test", "tools/test_fec_policy.c", "src/nb_fec_policy.c"):
        if token not in cmake:
            raise RuntimeError(f"P2 CTest integration missing: {token}")
    for token in ('"src/nb_v2_metadata.c"', '"tools/test_v2_metadata.c"',
                  '"src/nb_pmtu.c"', '"tools/test_pmtu.c"',
                  '"src/nb_fec_policy.c"'):
        if token not in deploy:
            raise RuntimeError(f"P2 remote build input missing: {token}")
    for token in ("P2-A UDP/PMTU", "P2-B FEC", "P2-C CI", "Xray-core"):
        if token not in roadmap:
            raise RuntimeError(f"P2 scope definition missing: {token}")
    print("P2-A/B/C KICKOFF GATE PASS")


if __name__ == "__main__":
    main()
