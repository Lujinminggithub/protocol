#!/usr/bin/env python3
"""候选媒体突发热更新脚本的本地单元测试。"""
from __future__ import annotations

import importlib.util
import math
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


BASELINE = b"tenant-v3 nbmobile 256 64 5000 5000 0 625000 625000\n"


def pressure_sample(value: int = 7) -> dict:
    workers = {}
    for role in ("entry", "middle", "exit"):
        for worker in ("0", "1"):
            key = f"{role}:{worker}"
            workers[key] = {
                "role": role, "worker": worker, "release_id": "candidate-release",
                "transport_generation": 2, "boot_id": f"boot-{key}",
                "queue_pressure_dropped": value, "sampled_at_utc": "2026-09-04T12:00:00Z",
            }
    return {"workers": workers}


class FakeRollout:
    def __init__(self, module, failure: str | None = None, exit_original: bytes | None = None) -> None:
        self.module = module
        self.failure = failure
        self.files = {"entry": BASELINE, "exit": exit_original or BASELINE}
        self.events: list[tuple[str, str]] = []
        self.requests: list[dict] = []
        self.collects = 0
        self.staged: dict[tuple[str, str], bytes] = {}
        self.failed = False

    def stage_directory(self, role: str, directory: str) -> None:
        self.events.append(("stage", role))

    def remote_read(self, role: str) -> bytes:
        self.events.append(("read", role))
        return self.files[role]

    def remote_push(self, role: str, data: bytes, path: str) -> None:
        self.events.append(("push", role))
        self.staged[(role, path)] = data

    def publish(self, role: str, source: str) -> None:
        self.events.append(("publish", role))
        if self.failure == "publish" and role == "entry" and not self.failed:
            self.failed = True
            raise RuntimeError("injected publish failure")
        self.files[role] = self.staged[(role, source)]

    def invoke(self, role: str, command: str):
        if command.startswith("tenant prepare"):
            self.events.append(("prepare", role))
            if self.failure == "prepare" and role == "exit" and not self.failed:
                self.failed = True
                raise RuntimeError("injected prepare failure")
            return [{"status": "ok"}]
        if command.startswith("tenant commit"):
            self.events.append(("commit", role))
            if self.failure == "commit" and role == "exit" and not self.failed:
                self.failed = True
                raise RuntimeError("injected commit failure")
            return [{"status": "ok"}]
        if command.startswith("tenant abort"):
            self.events.append(("abort", role))
            return [{"status": "ok"}]
        if command == "tenant status":
            self.events.append(("status", role))
            if self.failure == "readback" and role == "exit" and not self.failed:
                self.failed = True
                raise RuntimeError("injected readback failure")
            return [{"fingerprint": self.module.tenant_fingerprint(self.files[role])}]
        raise AssertionError(f"unexpected control command: {command}")

    def runner(self, request: dict):
        self.requests.append(request)
        if request["kind"] == "media_burst" and self.failure == "burst":
            raise RuntimeError("injected burst failure")
        if request["kind"] == "sustained_probe":
            return {"average_kbps": 5001 if self.failure == "sustained" else 5000, "duration_s": 90}
        return {"planned_bps": 12000000, "planned_duration_ms": 500,
                "sent_bytes": 750000, "received_bytes": 742500, "elapsed_ms": 500}

    def collector(self):
        self.collects += 1
        return pressure_sample(8 if self.failure == "pressure" and self.collects == 2 else 7)

    def install(self) -> None:
        self.originals = {name: getattr(self.module, name) for name in
                          ("remote_read", "remote_push", "invoke", "publish", "stage_directory")}
        self.module.remote_read = self.remote_read
        self.module.remote_push = self.remote_push
        self.module.invoke = self.invoke
        self.module.publish = self.publish
        self.module.stage_directory = self.stage_directory

    def restore(self) -> None:
        for name, value in self.originals.items():
            setattr(self.module, name, value)


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

    def test_rejects_mismatched_original_files_before_any_remote_write(self) -> None:
        fake = FakeRollout(self.module, exit_original=b"tenant-v3 nbmobile 256 64 5000 5000 0 625000 1\n")
        fake.install()
        try:
            with self.assertRaisesRegex(ValueError, "不一致"):
                self.module.apply(execute=True, runner=fake.runner, collector=fake.collector)
        finally:
            fake.restore()

        self.assertEqual(fake.files["entry"], BASELINE)
        self.assertEqual(fake.files["exit"], b"tenant-v3 nbmobile 256 64 5000 5000 0 625000 1\n")
        self.assertFalse(any(kind in {"stage", "push", "prepare", "publish", "commit"}
                             for kind, _role in fake.events))

    def test_prepares_all_roles_before_any_commit_and_runs_synthetic_gates(self) -> None:
        fake = FakeRollout(self.module)
        fake.install()
        try:
            result = self.module.apply(execute=True, runner=fake.runner, collector=fake.collector)
        finally:
            fake.restore()

        self.assertEqual(result["status"], "committed")
        first_commit = next(index for index, item in enumerate(fake.events) if item[0] == "commit")
        prepared = [item for item in fake.events[:first_commit] if item[0] == "prepare"]
        self.assertEqual(prepared, [("prepare", "entry"), ("prepare", "exit")])
        self.assertEqual(fake.files["entry"], fake.files["exit"])
        self.assertEqual(fake.files["entry"], self.module.TARGET_RECORD.encode("ascii") + b"\n")
        self.assertEqual(result["synthetic"]["queue_pressure_delta"], 0)
        self.assertEqual(result["synthetic"]["configured_rate_kbps"], 5000)
        self.assertEqual(result["synthetic"]["average_kbps"], 5000)
        self.assertEqual(fake.requests, [
            {"kind": "media_burst", "rate_mbps": 12, "duration_ms": 500},
            {"kind": "sustained_probe", "duration_s": 90, "maximum_kbps": 5000},
        ])

    def test_synthetic_validation_accepts_complete_structured_evidence(self) -> None:
        requests = []

        def runner(request):
            requests.append(request)
            if request["kind"] == "media_burst":
                return {"planned_bps": 12000000, "planned_duration_ms": 500,
                        "sent_bytes": 750000, "received_bytes": 742500, "elapsed_ms": 500}
            return {"average_kbps": 5000.0, "duration_s": 90}

        samples = iter((pressure_sample(4), pressure_sample(4), pressure_sample(4)))
        result = self.module.run_synthetic_validation(runner, lambda: next(samples))

        self.assertEqual(result["queue_pressure_delta"], 0)
        self.assertEqual(requests[0]["rate_mbps"], 12)

    def test_synthetic_validation_rejects_fail_open_evidence(self) -> None:
        valid_burst = {"planned_bps": 12000000, "planned_duration_ms": 500,
                       "sent_bytes": 750000, "received_bytes": 742500, "elapsed_ms": 500}
        valid_sustained = {"average_kbps": 5000.0, "duration_s": 90}
        cases = {
            "noop": ({}, valid_sustained, lambda: pressure_sample(4)),
            "wrong_bytes": ({**valid_burst, "sent_bytes": 749999}, valid_sustained,
                            lambda: pressure_sample(4)),
            "lost_echo": ({**valid_burst, "received_bytes": 742499}, valid_sustained,
                          lambda: pressure_sample(4)),
            "wrong_planned_duration": ({**valid_burst, "planned_duration_ms": 499}, valid_sustained,
                                       lambda: pressure_sample(4)),
            "wrong_elapsed": ({**valid_burst, "elapsed_ms": 499}, valid_sustained,
                              lambda: pressure_sample(4)),
            "wrong_plan": ({**valid_burst, "planned_bps": 11999999}, valid_sustained,
                           lambda: pressure_sample(4)),
            "float_over_limit": (valid_burst, {**valid_sustained, "average_kbps": 5000.9},
                                   lambda: pressure_sample(4)),
            "too_low": (valid_burst, {**valid_sustained, "average_kbps": 4499},
                        lambda: pressure_sample(4)),
            "short_sustained": (valid_burst, {**valid_sustained, "duration_s": 89},
                                lambda: pressure_sample(4)),
            "infinite_sustained": (valid_burst, {**valid_sustained, "duration_s": math.inf},
                                    lambda: pressure_sample(4)),
            "missing_pressure": (valid_burst, valid_sustained, lambda: {}),
        }
        for name, (burst, sustained, collector) in cases.items():
            with self.subTest(name=name):
                def runner(request, burst=burst, sustained=sustained):
                    return burst if request["kind"] == "media_burst" else sustained

                with self.assertRaises(RuntimeError):
                    self.module.run_synthetic_validation(runner, collector)

    def test_synthetic_validation_rechecks_queue_after_sustained_probe(self) -> None:
        def runner(request):
            if request["kind"] == "media_burst":
                return {"planned_bps": 12000000, "planned_duration_ms": 500,
                        "sent_bytes": 750000, "received_bytes": 742500, "elapsed_ms": 500}
            return {"average_kbps": 5000, "duration_s": 90}

        samples = iter((pressure_sample(4), pressure_sample(4), pressure_sample(5)))
        with self.assertRaisesRegex(RuntimeError, "queue_pressure_dropped"):
            self.module.run_synthetic_validation(runner, lambda: next(samples))

    def test_queue_pressure_rejects_worker_identity_drift_counter_reset_or_missing_role(self) -> None:
        before = pressure_sample(7)
        cases = {
            "restart": lambda after: after["workers"]["exit:1"].__setitem__("boot_id", "restarted"),
            "counter_reset": lambda after: after["workers"]["middle:0"].__setitem__("queue_pressure_dropped", 6),
            "missing_role": lambda after: after["workers"].pop("entry:0"),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                after = pressure_sample(7)
                mutate(after)
                with self.assertRaises(RuntimeError):
                    self.module.compare_queue_pressure(before, after)

    def test_worker_observation_requires_health_metrics_health_identity_match(self) -> None:
        health = {"role": "exit", "worker": "1", "release_id": "candidate-release",
                  "transport_generation": 2, "boot_id": "boot-exit-1", "status": "ok"}
        metrics = {"role": "exit", "worker": "1", "release_id": "candidate-release",
                   "udp_errors": {"queue_pressure_dropped": 7}}
        result = self.module.validate_worker_observation("exit", "1", health, metrics, dict(health))
        self.assertEqual(result["queue_pressure_dropped"], 7)

        for field, value in (("boot_id", "restarted"), ("release_id", "other-release")):
            with self.subTest(field=field):
                changed = dict(health)
                changed[field] = value
                with self.assertRaises(RuntimeError):
                    self.module.validate_worker_observation("exit", "1", health, metrics, changed)

    def test_every_transaction_or_gate_failure_restores_old_files_and_fingerprints(self) -> None:
        for failure in ("prepare", "publish", "commit", "readback", "pressure", "burst", "sustained"):
            with self.subTest(failure=failure):
                fake = FakeRollout(self.module, failure=failure)
                fake.install()
                try:
                    with self.assertRaisesRegex(RuntimeError, "已恢复"):
                        self.module.apply(execute=True, runner=fake.runner, collector=fake.collector)
                finally:
                    fake.restore()

                self.assertEqual(fake.files["entry"], BASELINE)
                self.assertEqual(fake.files["exit"], BASELINE)
                self.assertIn(("abort", "entry"), fake.events)
                self.assertIn(("abort", "exit"), fake.events)
                for role in ("entry", "exit"):
                    self.assertEqual(self.module.tenant_fingerprint(fake.files[role]),
                                     self.module.tenant_fingerprint(BASELINE))


if __name__ == "__main__":
    unittest.main()
