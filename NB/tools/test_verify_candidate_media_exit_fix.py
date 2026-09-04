#!/usr/bin/env python3
"""Local contract tests for the candidate media/Exit acceptance verifier."""
from __future__ import annotations

import importlib.util
import math
import pathlib
import sys
import threading
import types
import unittest
from collections import deque
from unittest import mock


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
    def snapshot(at: str) -> dict:
        return {
            f"{item['role']}:{item['worker']}": {
                "release_id": release, "transport_generation": 2,
                "boot_id": item["boot_id"], "queue_pressure_dropped": 10,
                "sampled_at_utc": at,
            } for item in workers
        }

    return {
        "controlplane": {"deployment": deployment, "generation": 2, "signal_direct": False,
                         "test_window": {"started_at_utc": "2026-09-04T12:00:00Z",
                                         "finished_at_utc": "2026-09-04T12:10:00Z"}},
        "workers": workers,
        "tenant": {
            "entry": {"rate_up_kbps": 5000, "rate_down_kbps": 5000,
                      "burst_up_bytes": 2500000, "burst_down_bytes": 625000},
            "exit": {"rate_up_kbps": 5000, "rate_down_kbps": 5000,
                     "burst_up_bytes": 2500000, "burst_down_bytes": 625000},
        },
        "queue_pressure_before": snapshot("2026-09-04T12:00:10Z"),
        "queue_pressure_after_burst": snapshot("2026-09-04T12:02:00Z"),
        "queue_pressure_after_sustained": snapshot("2026-09-04T12:05:00Z"),
        "private_dns_probe": {
            "host": "2fio72ng.sg-fn.tiktok-row.net",
            "address": "10.105.212.98",
            "elapsed_ms": 25,
            "result": "private-unreachable",
            "close_reason": "target-private-unreachable",
            "event_at_utc": "2026-09-04T12:01:00Z",
            "exit_connectivity_before": {
                "worker_key": "exit:0", "release_id": release, "deployment": deployment,
                "boot_id": "boot-exit-0", "observed_at": "2026-09-04T12:00:30Z",
                "dns_private_rejected": 0, "dns_failures": 0,
            },
            "exit_connectivity_after": {
                "worker_key": "exit:0", "release_id": release, "deployment": deployment,
                "boot_id": "boot-exit-0", "observed_at": "2026-09-04T12:01:30Z",
                "dns_private_rejected": 1, "dns_failures": 0,
            },
        },
        "synthetic_validation": {
            "burst": {"planned_bps": 12000000, "planned_duration_ms": 500,
                      "sent_bytes": 750000, "received_bytes": 742500, "elapsed_ms": 500},
            "sustained": {"duration_s": 90, "average_kbps": 4500},
        },
        "phone_validation": {
            "click_at": "2026-09-04T12:00:00Z",
            "enter_at": "2026-09-04T12:00:02Z",
            "target_zero_stalls": True,
            "state_events": [
                {"state": "red", "at_utc": "2026-09-04T12:00:03Z"},
                {"state": "yellow", "at_utc": "2026-09-04T12:00:04Z"},
                {"state": "green", "at_utc": "2026-09-04T12:00:05Z"},
            ],
            "viewer_events": [
                {"kind": "nonstall", "observed_from_utc": "2026-09-04T12:00:06Z",
                 "observed_to_utc": "2026-09-04T12:00:07Z", "duration_ms": 1000},
                {"kind": "nonstall", "observed_from_utc": "2026-09-04T12:00:08Z",
                 "observed_to_utc": "2026-09-04T12:00:09Z", "duration_ms": 1000},
            ],
        },
    }


class CandidateVerifierTests(unittest.TestCase):
    def test_native_probe_uses_configured_socks_port(self):
        calls = []
        control = mock.Mock()
        fake_deploy = types.SimpleNamespace(
            DEFAULT_SOCKS_PORT=1089,
            _role_host=lambda role: {"host": "192.0.2.10"},
        )
        fake_udp_probe = types.SimpleNamespace(
            udp_associate=lambda host, port: (calls.append((host, port)) or
                                               (control, ("192.0.2.10", 22000))),
        )
        udp = mock.Mock()
        with mock.patch.dict(sys.modules, {"deploy": fake_deploy, "udp_e2e_probe": fake_udp_probe}), \
                mock.patch.object(self.module, "_resolve_udp_relay", return_value=("192.0.2.10", 22000)), \
                mock.patch.object(self.module.socket, "socket", return_value=udp):
            state = self.module.NativeProbeAdapter().open()
        self.assertEqual(calls, [("192.0.2.10", 1089)])
        self.assertIs(state.control, control)
        udp.bind.assert_called_once_with(("0.0.0.0", 0))

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
        evidence["queue_pressure_after_burst"]["entry:0"]["queue_pressure_dropped"] = 11
        with self.assertRaisesRegex(ValueError, "queue_pressure_dropped"):
            self.module.validate_acceptance(evidence)

        evidence = valid_evidence()
        evidence["queue_pressure_after_sustained"].pop("exit:1")
        with self.assertRaisesRegex(ValueError, "queue_pressure"):
            self.module.validate_acceptance(evidence)

        evidence = valid_evidence()
        evidence["queue_pressure_after_burst"]["middle:1"]["boot_id"] = "restarted"
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
            "missing_time": lambda snapshot: snapshot.pop("observed_at"),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                evidence = valid_evidence()
                mutate(evidence["private_dns_probe"]["exit_connectivity_after"])
                with self.assertRaisesRegex(ValueError, "私网|证据"):
                    self.module.validate_acceptance(evidence)

    def test_acceptance_rejects_private_probe_observation_outside_event_window(self) -> None:
        for target, value in (("before", "2020-01-01T00:00:00Z"),
                              ("after", "2026-09-04T12:00:45Z")):
            with self.subTest(target=target):
                evidence = valid_evidence()
                evidence["private_dns_probe"][f"exit_connectivity_{target}"]["observed_at"] = value
                with self.assertRaisesRegex(ValueError, "私网"):
                    self.module.validate_acceptance(evidence)

    def test_parse_timestamp_requires_timezone_aware_utc(self) -> None:
        for timestamp in ("2026-09-04T12:00:00", "2026-09-04T20:00:00+08:00"):
            with self.subTest(timestamp=timestamp):
                with self.assertRaisesRegex(ValueError, "UTC"):
                    self.module._parse_timestamp(timestamp, "test.timestamp")

    def test_all_acceptance_window_timestamps_require_utc(self) -> None:
        cases = {
            "window_started": lambda evidence: evidence["controlplane"]["test_window"].__setitem__(
                "started_at_utc", "2026-09-04T20:00:00+08:00"),
            "window_finished": lambda evidence: evidence["controlplane"]["test_window"].__setitem__(
                "finished_at_utc", "2026-09-04T20:10:00+08:00"),
            "queue_before": lambda evidence: evidence["queue_pressure_before"]["entry:0"].__setitem__(
                "sampled_at_utc", "2026-09-04T20:00:10+08:00"),
            "queue_after_burst": lambda evidence: evidence["queue_pressure_after_burst"]["middle:0"].__setitem__(
                "sampled_at_utc", "2026-09-04T20:02:00+08:00"),
            "queue_after_sustained": lambda evidence: evidence["queue_pressure_after_sustained"]["exit:0"].__setitem__(
                "sampled_at_utc", "2026-09-04T20:05:00+08:00"),
            "private_before": lambda evidence: evidence["private_dns_probe"]["exit_connectivity_before"].__setitem__(
                "observed_at", "2026-09-04T20:00:30+08:00"),
            "private_event": lambda evidence: evidence["private_dns_probe"].__setitem__(
                "event_at_utc", "2026-09-04T20:01:00+08:00"),
            "private_after": lambda evidence: evidence["private_dns_probe"]["exit_connectivity_after"].__setitem__(
                "observed_at", "2026-09-04T20:01:30+08:00"),
            "phone_click": lambda evidence: evidence["phone_validation"].__setitem__(
                "click_at", "2026-09-04T20:00:00+08:00"),
            "phone_enter": lambda evidence: evidence["phone_validation"].__setitem__(
                "enter_at", "2026-09-04T20:00:02+08:00"),
            "phone_state": lambda evidence: evidence["phone_validation"]["state_events"][0].__setitem__(
                "at_utc", "2026-09-04T20:00:03+08:00"),
            "viewer_started": lambda evidence: evidence["phone_validation"]["viewer_events"][0].__setitem__(
                "observed_from_utc", "2026-09-04T20:00:06+08:00"),
            "viewer_finished": lambda evidence: evidence["phone_validation"]["viewer_events"][0].__setitem__(
                "observed_to_utc", "2026-09-04T20:00:07+08:00"),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                evidence = valid_evidence()
                mutate(evidence)
                with self.assertRaisesRegex(ValueError, "UTC"):
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

    def test_incomplete_phone_timestamps_require_utc_before_pending(self) -> None:
        cases = {
            "click": ({"click_at": "2026-09-04T12:00:00"},
                      {"click_at": "2026-09-04T12:00:00Z"}),
            "enter": ({"enter_at": "2026-09-04T20:00:02+08:00"},
                      {"enter_at": "2026-09-04T12:00:02Z"}),
            "state": ({"state_events": [{"at_utc": "2026-09-04T12:00:03"}]},
                      {"state_events": [{"at_utc": "2026-09-04T12:00:03Z"}]}),
            "viewer_at": ({"viewer_events": [{"at_utc": "2026-09-04T20:00:06+08:00"}]},
                          {"viewer_events": [{"at_utc": "2026-09-04T12:00:06Z"}]}),
            "viewer_from": ({"viewer_events": [{"observed_from_utc": "2026-09-04T12:00:06"}]},
                            {"viewer_events": [{"observed_from_utc": "2026-09-04T12:00:06Z"}]}),
            "viewer_to": ({"viewer_events": [{"observed_to_utc": "2026-09-04T20:00:07+08:00"}]},
                          {"viewer_events": [{"observed_to_utc": "2026-09-04T12:00:07Z"}]}),
        }
        for name, (invalid_phone, valid_incomplete_phone) in cases.items():
            with self.subTest(name=name, phase="invalid"):
                evidence = valid_evidence()
                evidence["phone_validation"] = invalid_phone
                with self.assertRaisesRegex(ValueError, "UTC"):
                    self.module.validate_acceptance(evidence)
            with self.subTest(name=name, phase="pending"):
                evidence = valid_evidence()
                evidence["phone_validation"] = valid_incomplete_phone
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

        evidence = valid_evidence()
        evidence["synthetic_validation"]["sustained"]["duration_s"] = math.inf
        with self.assertRaisesRegex(ValueError, "合成"):
            self.module.validate_acceptance(evidence)

    def test_final_acceptance_rejects_phone_event_outside_window_or_zero_target_stall(self) -> None:
        evidence = valid_evidence()
        evidence["phone_validation"]["viewer_events"][0]["kind"] = "stall"
        with self.assertRaisesRegex(ValueError, "手机"):
            self.module.validate_acceptance(evidence)

        evidence = valid_evidence()
        evidence["phone_validation"]["viewer_events"][0]["observed_to_utc"] = "2026-09-04T12:11:00Z"
        with self.assertRaisesRegex(ValueError, "手机"):
            self.module.validate_acceptance(evidence)

        evidence = valid_evidence()
        evidence["private_dns_probe"]["event_at_utc"] = "2026-09-04T12:11:00Z"
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

    def test_native_probe_packet_targets_only_internal_echo(self) -> None:
        packet = self.module.encode_native_probe_datagram(7, b"payload")

        self.assertIn(b"nb-probe-echo.internal", packet)
        self.assertTrue(packet.endswith((7).to_bytes(8, "big") + b"payload"))

    def test_native_probe_decodes_domain_reply_and_requires_exact_host(self) -> None:
        host = b"nb-probe-echo.internal"
        payload = (7).to_bytes(8, "big") + b"echo"
        reply = b"\x00\x00\x00\x03" + bytes([len(host)]) + host + (9).to_bytes(2, "big") + payload
        sequence, echoed = self.module.decode_native_probe_response(reply)
        self.assertEqual((sequence, echoed), (7, payload))

        invalid_hosts = (
            b"nb-probe-echo.internal.",
            b"nb-probe-echo.internal..",
            b"NB-PROBE-ECHO.INTERNAL",
            b"wrong-probe.internal",
        )
        for invalid_host in invalid_hosts:
            invalid = (b"\x00\x00\x00\x03" + bytes([len(invalid_host)]) + invalid_host +
                       (9).to_bytes(2, "big") + payload)
            with self.subTest(host=invalid_host):
                with self.assertRaises(ValueError):
                    self.module.decode_native_probe_response(invalid)

        for invalid in (reply[:-(len(payload) - 7)],
                        reply[:-len(payload)-2] + (10).to_bytes(2, "big") + payload):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    self.module.decode_native_probe_response(invalid)

    def test_native_probe_ignores_echoes_from_any_peer_other_than_exact_relay(self) -> None:
        class EchoSocket:
            def __init__(self, peer: tuple[str, int]) -> None:
                self.peer = peer
                self.replies: deque[tuple[bytes, tuple[str, int]]] = deque()
                self.lock = threading.Lock()

            def sendto(self, data: bytes, _relay: tuple[str, int]) -> int:
                with self.lock:
                    self.replies.append((data, self.peer))
                return len(data)

            def recvfrom(self, _size: int) -> tuple[bytes, tuple[str, int]]:
                with self.lock:
                    if self.replies:
                        return self.replies.popleft()
                raise BlockingIOError

        relay = ("127.0.0.1", 1081)
        for name, peer in (("wrong_ip", ("127.0.0.2", relay[1])),
                           ("wrong_port", (relay[0], relay[1] + 1))):
            with self.subTest(name=name):
                state = self.module._NativeProbeState(object(), EchoSocket(peer), relay)
                with self.assertRaisesRegex(RuntimeError, "99%"):
                    self.module.NativeProbeAdapter._measure(
                        state, 12_000_000, 0.5, "media_burst")

    def test_native_probe_resolves_domain_relay_and_uses_one_numeric_peer(self) -> None:
        numeric_relay = ("127.0.0.1", 1081)

        def ipv4_udp_resolution(host: str, port: int, family: int = 0,
                                socktype: int = 0, *_args, **_kwargs):
            if (host, port, family, socktype) != (
                    "relay.example", 1081, self.module.socket.AF_INET,
                    self.module.socket.SOCK_DGRAM):
                return []
            return [(self.module.socket.AF_INET, self.module.socket.SOCK_DGRAM,
                     self.module.socket.IPPROTO_UDP, "", numeric_relay)]

        class EchoSocket:
            def __init__(self) -> None:
                self.replies: deque[tuple[bytes, tuple[str, int]]] = deque()
                self.lock = threading.Lock()

            def sendto(self, data: bytes, relay: tuple[str, int]) -> int:
                if relay != numeric_relay:
                    raise OSError("relay was not pinned to numeric IPv4")
                with self.lock:
                    self.replies.append((b"malformed", ("127.0.0.2", relay[1])))
                    self.replies.append((data, numeric_relay))
                return len(data)

            def recvfrom(self, _size: int) -> tuple[bytes, tuple[str, int]]:
                with self.lock:
                    if self.replies:
                        return self.replies.popleft()
                raise BlockingIOError

        with mock.patch.object(self.module.socket, "getaddrinfo",
                               side_effect=ipv4_udp_resolution):
            relay = self.module._resolve_udp_relay(("relay.example", 1081))
        self.assertEqual(relay, numeric_relay)

        state = self.module._NativeProbeState(object(), EchoSocket(), relay)
        result = self.module.NativeProbeAdapter._measure(
            state, 12_000_000, 0.5, "media_burst")
        self.assertEqual(result["sent_bytes"], 750000)
        self.assertEqual(result["received_bytes"], 750000)

    def test_native_probe_relay_resolution_fails_closed(self) -> None:
        cases = {
            "empty": (("", 1081), []),
            "ipv6": (("::1", 1081), [
                (self.module.socket.AF_INET6, self.module.socket.SOCK_DGRAM,
                 self.module.socket.IPPROTO_UDP, "", ("::1", 1081, 0, 0)),
            ]),
            "no_address": (("relay.example", 1081), []),
        }
        for name, (relay, resolved) in cases.items():
            with self.subTest(name=name):
                with mock.patch.object(self.module.socket, "getaddrinfo", return_value=resolved):
                    with self.assertRaises(RuntimeError):
                        self.module._resolve_udp_relay(relay)

        with mock.patch.object(self.module.socket, "getaddrinfo",
                               side_effect=self.module.socket.gaierror("lookup failed")):
            with self.assertRaises(RuntimeError):
                self.module._resolve_udp_relay(("relay.example", 1081))


if __name__ == "__main__":
    unittest.main()
