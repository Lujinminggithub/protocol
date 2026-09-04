#!/usr/bin/env python3
"""候选媒体突发热更新脚本的本地单元测试。"""
from __future__ import annotations

import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "build" / "deploy_candidate_media_burst.py"


def load_module():
    spec = importlib.util.spec_from_file_location("deploy_candidate_media_burst", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MediaBurstTests(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_module()

    def test_render_replaces_only_expected_tenant_v3_record(self) -> None:
        original = (
            b"# generated\n"
            b"tenant-v3 nbmobile 256 64 5000 5000 0 625000 625000\n"
            b"tenant-v3 other 1 1 1000 1000 0 125000 125000\n"
        )

        rendered = self.module.render_tenant_config(original)

        self.assertIn(self.module.TARGET_RECORD.encode("ascii") + b"\n", rendered)
        self.assertIn(b"tenant-v3 other 1 1 1000 1000 0 125000 125000\n", rendered)
        self.assertNotIn(b"tenant-v3 nbmobile 256 64 5000 5000 0 625000 625000", rendered)

    def test_render_rejects_unexpected_sustained_rate_or_burst(self) -> None:
        original = b"tenant-v3 nbmobile 256 64 6000 5000 0 625000 625000\n"

        with self.assertRaisesRegex(ValueError, "tenant-v3"):
            self.module.render_tenant_config(original)

    def test_readback_requires_same_generation_and_target_record(self) -> None:
        expected = "0011223344556677"
        responses = [{"fingerprint": expected}, {"fingerprint": expected}]

        result = self.module.validate_control_readback("entry", responses, expected)

        self.assertEqual(result["workers"], 2)
        with self.assertRaisesRegex(RuntimeError, "fingerprint"):
            self.module.validate_control_readback("entry", [{"fingerprint": "bad"}], expected)

    def test_dry_run_is_local_and_has_no_remote_effect(self) -> None:
        original_connect = self.module.deploy.connect
        try:
            self.module.deploy.connect = lambda role: self.fail(f"unexpected connection: {role}")
            result = self.module.apply(execute=False)
        finally:
            self.module.deploy.connect = original_connect

        self.assertEqual(result["status"], "preflight")
        self.assertEqual(result["record"], self.module.TARGET_RECORD)
        self.assertEqual(result["instance"], "gz2-hk2-kz-00002_1")


if __name__ == "__main__":
    unittest.main()
