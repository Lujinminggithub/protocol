#!/usr/bin/env python3
"""Local contract tests for the candidate media/Exit acceptance verifier."""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "build" / "verify_candidate_media_exit_fix.py"


def load_module():
    spec = importlib.util.spec_from_file_location("verify_candidate_media_exit_fix", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load candidate verifier")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def valid_evidence() -> dict:
    deployment = "candidate-deployment-20260904"
    release = "candidate-release-20260904"
    workers = []
    for role in ("entry", "middle", "exit"):
        for worker in ("0", "1"):
            metrics = {"udp_errors": {"queue_pressure_dropped": 10}}
            if role == "exit":
                metrics["exit_connectivity"] = {
                    "dns_requests": 4,
                    "dns_failures": 1,
                    "dns_private_rejected": 1,
                    "dns_latency_max_us": 1200,
                    "target_connect_timeouts": 0,
                    "target_connect_latency_max_us": 90000,
                }
            workers.append({
                "role": role,
                "worker": worker,
                "status": "ok",
                "release_id": release,
                "deployment": deployment,
                "generation": 2,
                "boot_id": f"boot-{role}-{worker}",
                "metrics": metrics,
            })
    return {
        "controlplane": {"deployment": deployment, "generation": 2, "signal_direct": False},
        "workers": workers,
        "tenant": {
            "entry": {"rate_up_kbps": 5000, "rate_down_kbps": 5000,
                      "burst_up_bytes": 2500000, "burst_down_bytes": 625000},
            "exit": {"rate_up_kbps": 5000, "rate_down_kbps": 5000,
                     "burst_up_bytes": 2500000, "burst_down_bytes": 625000},
        },
        "queue_pressure_before": {
            f"{item['role']}:{item['worker']}": {
                "release_id": release, "transport_generation": 2,
                "boot_id": item["boot_id"], "queue_pressure_dropped": 10,
            } for item in workers
        },
        "queue_pressure_after": {
            f"{item['role']}:{item['worker']}": {
                "release_id": release, "transport_generation": 2,
                "boot_id": item["boot_id"], "queue_pressure_dropped": 10,
            } for item in workers
        },
        "private_dns_probe": {
            "host": "2fio72ng.sg-fn.tiktok-row.net",
            "address": "10.105.212.98",
            "elapsed_ms": 25,
            "result": "private-unreachable",
            "close_reason": "target-private-unreachable",
            "exit_connectivity_before": {
                "worker_key": "exit:0", "release_id": release, "deployment": deployment,
                "boot_id": "boot-exit-0", "sampled_at_utc": "2026-09-04T12:00:00Z",
                "dns_private_rejected": 0, "dns_failures": 0,
            },
            "exit_connectivity_after": {
                "worker_key": "exit:0", "release_id": release, "deployment": deployment,
                "boot_id": "boot-exit-0", "sampled_at_utc": "2026-09-04T12:00:01Z",
                "dns_private_rejected": 1, "dns_failures": 0,
            },
        },
        "synthetic_validation": {
            "burst": {"planned_bps": 12000000, "planned_duration_ms": 500,
                      "sent_bytes": 750000, "received_bytes": 742500, "elapsed_ms": 500},
            "sustained": {"duration_s": 90, "average_kbps": 4500},
        },
        "phone_validation": {
            "click_at_utc": "2026-09-04T12:00:00Z",
            "enter_at_utc": "2026-09-04T12:00:02Z",
            "state_timeline": [
                {"state": "red", "at_utc": "2026-09-04T12:00:03Z"},
                {"state": "yellow", "at_utc": "2026-09-04T12:00:04Z"},
                {"state": "green", "at_utc": "2026-09-04T12:00:05Z"},
            ],
            "viewer_events": [
                {"event": "stall", "at_utc": "2026-09-04T12:00:06Z", "duration_ms": 120},
                {"event": "stall", "at_utc": "2026-09-04T12:00:07Z", "duration_ms": 90},
            ],
        },
    }


class CandidateVerifierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_module()

    def test_acceptance_returns_six_worker_summary_for_complete_evidence(self) -> None:
        result = self.module.validate_acceptance(valid_evidence())

        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["workers"], 6)
        self.assertEqual(result["generation"], 2)
        self.assertEqual(result["queue_pressure_delta"], 0)
        self.assertEqual(result["tenant"]["burst_up_bytes"], 2500000)

    def test_acceptance_fails_closed_when_worker_or_release_evidence_is_incomplete(self) -> None:
        cases = {
            "missing_worker": lambda evidence: evidence["workers"].pop(),
            "wrong_generation": lambda evidence: evidence["workers"].__setitem__(0, {
                **evidence["workers"][0], "generation": 1}),
            "mixed_release": lambda evidence: evidence["workers"].__setitem__(0, {
                **evidence["workers"][0], "release_id": "different-release"}),
            "signal_direct": lambda evidence: evidence["controlplane"].__setitem__("signal_direct", True),
            "missing_exit_connectivity": lambda evidence: evidence["workers"][4]["metrics"].pop("exit_connectivity"),
            "wrong_tenant": lambda evidence: evidence["tenant"]["exit"].__setitem__("rate_up_kbps", 5001),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                evidence = valid_evidence()
                mutate(evidence)
                with self.assertRaisesRegex(ValueError, "证据"):
                    self.module.validate_acceptance(evidence)

    def test_acceptance_requires_zero_queue_pressure_delta_per_worker(self) -> None:
        evidence = valid_evidence()
        evidence["queue_pressure_after"]["entry:0"]["queue_pressure_dropped"] = 11
        with self.assertRaisesRegex(ValueError, "queue_pressure_dropped"):
            self.module.validate_acceptance(evidence)

        evidence = valid_evidence()
        evidence["queue_pressure_after"].pop("exit:1")
        with self.assertRaisesRegex(ValueError, "queue_pressure"):
            self.module.validate_acceptance(evidence)

        evidence = valid_evidence()
        evidence["queue_pressure_after"]["middle:1"]["boot_id"] = "restarted"
        with self.assertRaisesRegex(ValueError, "queue_pressure"):
            self.module.validate_acceptance(evidence)

    def test_acceptance_requires_private_tiktok_fast_failure_and_counter_increment(self) -> None:
        cases = {
            "other_domain": lambda probe: probe.__setitem__("host", "rtc-access.tiktokv.com"),
            "public_address": lambda probe: probe.__setitem__("address", "8.8.8.8"),
            "loopback_is_not_rfc1918": lambda probe: probe.__setitem__("address", "127.0.0.1"),
            "slow": lambda probe: probe.__setitem__("elapsed_ms", 1001),
            "one_second_is_not_fast": lambda probe: probe.__setitem__("elapsed_ms", 1000),
            "wrong_reason": lambda probe: probe.__setitem__("close_reason", "target-connect-timeout"),
            "counter_not_increased": lambda probe: probe["exit_connectivity_after"].__setitem__(
                "dns_private_rejected", 0),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                evidence = valid_evidence()
                mutate(evidence["private_dns_probe"])
                with self.assertRaisesRegex(ValueError, "私网|证据"):
                    self.module.validate_acceptance(evidence)

    def test_acceptance_rejects_private_probe_identity_or_window_drift(self) -> None:
        cases = {
            "worker": lambda snapshot: snapshot.__setitem__("worker_key", "exit:1"),
            "release": lambda snapshot: snapshot.__setitem__("release_id", "other-release"),
            "boot": lambda snapshot: snapshot.__setitem__("boot_id", "restarted"),
            "missing_time": lambda snapshot: snapshot.pop("sampled_at_utc"),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                evidence = valid_evidence()
                mutate(evidence["private_dns_probe"]["exit_connectivity_after"])
                with self.assertRaisesRegex(ValueError, "私网|证据"):
                    self.module.validate_acceptance(evidence)

    def test_missing_phone_timeline_is_pending_not_passed(self) -> None:
        evidence = valid_evidence()
        evidence.pop("phone_validation")

        result = self.module.validate_acceptance(evidence)

        self.assertEqual(result["status"], "pending_phone_validation")

        evidence = valid_evidence()
        evidence["phone_validation"] = {"click_at_utc": "2026-09-04T12:00:00Z"}
        result = self.module.validate_acceptance(evidence)
        self.assertEqual(result["status"], "pending_phone_validation")

    def test_final_acceptance_requires_complete_synthetic_and_phone_evidence(self) -> None:
        for key in ("synthetic_validation", "phone_validation"):
            with self.subTest(key=key):
                evidence = valid_evidence()
                evidence.pop(key)
                result = self.module.validate_acceptance(evidence)
                self.assertNotEqual(result["status"], "passed")

        evidence = valid_evidence()
        evidence["synthetic_validation"]["burst"]["received_bytes"] = 742499
        with self.assertRaisesRegex(ValueError, "合成"):
            self.module.validate_acceptance(evidence)

    def test_runner_request_rejects_unknown_or_invalid_values_before_opening_probe(self) -> None:
        for request in (
            {},
            {"kind": "media_burst", "rate_mbps": 11, "duration_ms": 500},
            {"kind": "media_burst", "rate_mbps": 12, "duration_ms": 499},
            {"kind": "sustained_probe", "duration_s": 89, "maximum_kbps": 5000},
            {"kind": "unrelated"},
        ):
            with self.subTest(request=request):
                with self.assertRaises(ValueError):
                    self.module.parse_runner_request(request)

    def test_isolated_probe_always_closes_temporary_echo_after_traffic_error(self) -> None:
        events = []

        class Adapter:
            def open_echo(self):
                events.append("open")
                return "temporary-echo"

            def run_traffic(self, request, echo):
                assert echo == "temporary-echo"
                events.append("traffic")
                raise RuntimeError("injected traffic failure")

            def close_echo(self, echo):
                assert echo == "temporary-echo"
                events.append("close")

        with self.assertRaisesRegex(RuntimeError, "injected traffic failure"):
            self.module.run_isolated_probe({"kind": "media_burst", "rate_mbps": 12,
                                            "duration_ms": 500}, Adapter())
        self.assertEqual(events, ["open", "traffic", "close"])

    def test_isolated_probe_fails_when_whitelist_restore_fails(self) -> None:
        events = []

        class Adapter:
            def open_echo(self):
                events.append("echo-open")
                return "echo"

            def run_traffic(self, request, echo):
                events.append("traffic")
                return {"ok": True}

            def close_echo(self, echo):
                events.append("echo-close")

        class Lease:
            def install(self):
                events.append("whitelist-install")

            def restore(self):
                events.append("whitelist-restore")
                raise RuntimeError("restore failed")

        with self.assertRaisesRegex(RuntimeError, "清理"):
            self.module.run_isolated_probe({"kind": "media_burst", "rate_mbps": 12,
                                            "duration_ms": 500}, Adapter(), Lease())
        self.assertEqual(events, ["whitelist-install", "echo-open", "traffic", "echo-close",
                                  "whitelist-restore"])

    def test_echo_kill_is_strict_and_never_masks_a_remote_failure(self) -> None:
        commands = []

        self.module.strict_kill_echo("connection", 321, lambda connection, command, tmo: commands.append(command))

        self.assertEqual(commands, ["kill -- -321"])
        with self.assertRaisesRegex(RuntimeError, "kill failed"):
            self.module.strict_kill_echo("connection", 321,
                                         lambda connection, command, tmo: (_ for _ in ()).throw(
                                             RuntimeError("kill failed")))


if __name__ == "__main__":
    unittest.main()
