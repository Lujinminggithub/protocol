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
        "queue_pressure_before": {f"{item['role']}:{item['worker']}": 10 for item in workers},
        "queue_pressure_after": {f"{item['role']}:{item['worker']}": 10 for item in workers},
        "private_dns_probe": {
            "host": "2fio72ng.sg-fn.tiktok-row.net",
            "address": "10.105.212.98",
            "elapsed_ms": 25,
            "result": "private-unreachable",
            "close_reason": "target-private-unreachable",
            "exit_connectivity_before": {"dns_private_rejected": 0, "dns_failures": 0},
            "exit_connectivity_after": {"dns_private_rejected": 1, "dns_failures": 0},
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
        evidence["queue_pressure_after"]["entry:0"] = 11
        with self.assertRaisesRegex(ValueError, "queue_pressure_dropped"):
            self.module.validate_acceptance(evidence)

        evidence = valid_evidence()
        evidence["queue_pressure_after"].pop("exit:1")
        with self.assertRaisesRegex(ValueError, "queue_pressure"):
            self.module.validate_acceptance(evidence)

    def test_acceptance_requires_private_tiktok_fast_failure_and_counter_increment(self) -> None:
        cases = {
            "other_domain": lambda probe: probe.__setitem__("host", "rtc-access.tiktokv.com"),
            "public_address": lambda probe: probe.__setitem__("address", "8.8.8.8"),
            "loopback_is_not_rfc1918": lambda probe: probe.__setitem__("address", "127.0.0.1"),
            "slow": lambda probe: probe.__setitem__("elapsed_ms", 1001),
            "wrong_reason": lambda probe: probe.__setitem__("close_reason", "target-connect-timeout"),
            "counter_not_increased": lambda probe: probe["exit_connectivity_after"].__setitem__(
                "dns_private_rejected", 0),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                evidence = valid_evidence()
                mutate(evidence["private_dns_probe"])
                with self.assertRaisesRegex(ValueError, "私网"):
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


if __name__ == "__main__":
    unittest.main()
