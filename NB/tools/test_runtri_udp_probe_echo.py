#!/usr/bin/env python3
import contextlib
import io
import socket
import sys
import unittest
from unittest import mock

import runtri_udp_probe_echo as probe


class RuntriUdpProbeEchoTest(unittest.TestCase):
    def test_password_is_read_from_stdin(self):
        self.assertEqual(probe.read_password(io.StringIO("nb-test-password\n")), b"nb-test-password")
        with self.assertRaises(RuntimeError):
            probe.read_password(io.StringIO("\n"))

    def test_password_is_not_accepted_from_argv(self):
        with mock.patch.object(sys, "argv", ["probe", "--entry-port", "1", "--username", "nbtest",
                                               "--password", "nb-test-password", "--control", "entry"]):
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    probe.main()

    def test_domain_relay_resolves_to_numeric_ipv4(self):
        with mock.patch.object(probe.socket, "getaddrinfo", return_value=[
            (socket.AF_INET6, socket.SOCK_DGRAM, 0, "", ("::1", 9, 0, 0)),
            (socket.AF_INET, socket.SOCK_DGRAM, 0, "", ("127.0.0.9", 9)),
        ]):
            self.assertEqual(probe.numeric_ipv4_relay("relay.internal", 9), ("127.0.0.9", 9))

    def test_unexpected_udp_peer_is_not_accepted(self):
        class DatagramSocket:
            def __init__(self):
                self.calls = 0

            def recvfrom(self, _size):
                self.calls += 1
                if self.calls == 1:
                    return b"spoof", ("127.0.0.8", 9)
                return b"valid", ("127.0.0.9", 9)

        with self.assertRaisesRegex(RuntimeError, "unexpected UDP peer"):
            probe.recv_expected_peer(DatagramSocket(), ("127.0.0.9", 9), 2)

    def test_control_baseline_requires_each_node_to_be_zero_twice(self):
        with mock.patch.object(probe, "udp_sessions", side_effect=[0, 0, 0, 1]):
            with self.assertRaisesRegex(RuntimeError, "unstable"):
                probe.require_stable_zero(["entry", "middle"])
        with mock.patch.object(probe, "udp_sessions", side_effect=[1, 0, 0, 0]):
            with self.assertRaisesRegex(RuntimeError, "unstable"):
                probe.require_stable_zero(["entry", "middle"])

    def test_nonzero_cannot_be_cancelled_by_another_node(self):
        self.assertFalse(probe.exactly_zero([1, 0, 0]))
        self.assertFalse(probe.exactly_zero([0, 0, 1]))
        self.assertTrue(probe.exactly_zero([0, 0, 0]))


if __name__ == "__main__":
    unittest.main()
