#!/usr/bin/env python3
import contextlib
import io
import json
import os
import sys
import threading
import types

import line_probe
from line_probe import (ENTRY_LOCAL_PROBE_SCRIPT, evaluate_admission, fnv1a64,
                        control_link_sample, parse_entry_probe_output, parse_linkq, parse_ping,
                        recommend, recommend_mtu,
                        run_entry_local_probe, run_load_probe, run_probe_pair, summarize_linkq)


class FakeChannel:
    def shutdown_write(self):
        pass

    def recv_exit_status(self):
        return 0


class FakeStdin:
    def __init__(self):
        self.channel = FakeChannel()
        self.value = b""

    def write(self, value):
        self.value += value


class FakeStdout(list):
    def __init__(self, lines):
        super().__init__(lines)
        self.channel = FakeChannel()


class FakeStderr:
    def read(self):
        return b""


class FakeConnection:
    def __init__(self):
        self.stdin = FakeStdin()
        self.closed = False

    def exec_command(self, command, timeout):
        assert command.startswith("python3 -c ") and timeout >= 60
        return self.stdin, FakeStdout([
            b'NBPROBE_PROGRESS {"elapsed_s":5,"sent_bytes":625000,"achieved_mbps":1}\n',
            b'NBPROBE_RESULT {"bytes":625000,"elapsed_s":5,"achieved_mbps":1,'
            b'"integrity":"count-ok","origin":"entry-local"}\n',
        ]), FakeStderr()

    def close(self):
        self.closed = True


class FakeLogConnection:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


class FakeProbeSocket:
    def __init__(self):
        self.shutdown_how = None

    def sendall(self, value):
        pass

    def shutdown(self, how):
        self.shutdown_how = how

    def recv(self, size):
        return b"NBPROBE OK bytes=0 hash=0000000000000000\n"

    def close(self):
        pass


class Stopped:
    def is_set(self):
        return True


def execute_entry_downlink(payloads):
    class ProbeSocket:
        def __init__(self):
            self.responses = [b"\x05\x02", b"\x01\x00", b"\x05\x00\x00\x01", b"\0" * 6,
                              *payloads]

        def settimeout(self, _value):
            pass

        def sendall(self, _value):
            pass

        def recv(self, _size):
            return self.responses.pop(0) if self.responses else b""

        def close(self):
            pass

    fake_socket = types.SimpleNamespace(
        create_connection=lambda *_args, **_kwargs: ProbeSocket(),
        timeout=TimeoutError,
        SHUT_WR=1,
    )
    request = {"mode": "downlink", "socks_port": 1084, "username": "user",
               "password": "password", "size": 0, "target_mbps": 0.000032,
               "duration_s": 4, "io_timeout": 1}
    previous_socket = sys.modules.get("socket")
    previous_stdin = sys.stdin
    output = io.StringIO()
    try:
        sys.modules["socket"] = fake_socket
        sys.stdin = io.StringIO(json.dumps(request))
        with contextlib.redirect_stdout(output):
            exec(ENTRY_LOCAL_PROBE_SCRIPT, {})
    finally:
        sys.stdin = previous_stdin
        if previous_socket is None:
            sys.modules.pop("socket", None)
        else:
            sys.modules["socket"] = previous_socket
    line = next(value for value in output.getvalue().splitlines()
                if value.startswith("NBPROBE_RESULT "))
    return json.loads(line.removeprefix("NBPROBE_RESULT "))


def main() -> None:
    compile(ENTRY_LOCAL_PROBE_SCRIPT, "<entry-local-probe>", "exec")
    rejected_cache = {
        "_probe_cache_key": "same-key",
        "admission": {"status": "rejected"},
        "active_probe": {
            "uplink": {"integrity": "count-ok", "bytes": 1, "expected_bytes": 1},
            "downlink": {"integrity": "count-ok", "bytes": 1, "expected_bytes": 1, "complete": True},
        },
    }
    assert not line_probe.cache_candidate_reusable(rejected_cache, "same-key")
    admitted_cache = {
        "_probe_cache_key": "same-key",
        "admission": {"status": "admitted"},
        "active_probe": {
            "uplink": {"integrity": "count-ok", "bytes": 1, "expected_bytes": 1},
            "downlink": {"integrity": "count-ok", "bytes": 1, "expected_bytes": 1, "complete": True},
        },
    }
    assert line_probe.cache_candidate_reusable(admitted_cache, "same-key")
    assert "sock.shutdown(socket.SHUT_WR)" in ENTRY_LOCAL_PROBE_SCRIPT
    assert 'connect_socks("nb-probe-source.internal")' in ENTRY_LOCAL_PROBE_SCRIPT
    assert 'b"NBP2"' in ENTRY_LOCAL_PROBE_SCRIPT
    assert fnv1a64(b"abc") == 0xe71fa2190541574b
    partial = execute_entry_downlink([bytes([29, 42, 55, 68, 81, 94, 107, 120]), b""])
    assert partial["bytes"] == 8 and partial["expected_bytes"] == 16
    assert partial["complete"] is False and partial["ended_early"] is True
    assert partial["integrity"] == "count-ok" and partial["hash_algorithm"] == "sha256"
    assert abs(partial["achieved_mbps"] - 0.000016) < 0.0000001
    rejected_partial = evaluate_admission({"integrity": {"integrity": "ok"},
        "uplink": {"integrity": "count-ok", "achieved_mbps": 10},
        "downlink": partial}, 10.0)
    assert rejected_partial["status"] == "rejected"
    assert rejected_partial["reasons"] == ["insufficient-downlink"]
    try:
        execute_entry_downlink([bytes([29, 99, 55, 68]), b""])
    except RuntimeError as error:
        assert "integrity mismatch" in str(error)
    else:
        raise AssertionError("corrupt downlink payload was accepted")
    control_sample = control_link_sample([
        {"link": {"sent_packets": 800, "rtt_max_us": 4500, "jitter_max_us": 600,
                  "effective_loss_max_pct": 0.1, "spurious_total": 2,
                  "reorder_gap_max": 3, "reorder_delay_max_us": 12000,
                  "cwin_max_bytes": 262144, "blocked_connections": 0}},
        {"link": {"sent_packets": 500, "rtt_max_us": 5100, "jitter_max_us": 800,
                  "effective_loss_max_pct": 0.2, "spurious_total": 1,
                  "reorder_gap_max": 4, "reorder_delay_max_us": 15000,
                  "cwin_max_bytes": 524288, "blocked_connections": 1}},
    ])
    assert control_sample["sent"] == 1300 and control_sample["rtt"] == 5.1
    assert control_sample["reorder_gap"] == 4 and control_sample["block"] == 1
    byte_fallback = control_link_sample([
        {"link": {"samples": 1, "sent_packets": 2, "rtt_max_us": 4500,
                  "jitter_max_us": 600, "effective_loss_max_pct": 0.1,
                  "spurious_total": 0, "reorder_gap_max": 1,
                  "reorder_delay_max_us": 12000}},
        {"bytes": {"c2s": 1200000, "s2c": 0},
         "link": {"samples": 1, "sent_packets": 2, "rtt_max_us": 4500,
                  "jitter_max_us": 600, "effective_loss_max_pct": 0.1,
                  "spurious_total": 0, "reorder_gap_max": 1,
                  "reorder_delay_max_us": 12000}},
    ], previous_bytes=0)
    assert byte_fallback["sent"] >= 1000
    assert byte_fallback["bytes_delta"] == 1200000
    remote = parse_entry_probe_output([
        'NBPROBE_PROGRESS {"elapsed_s":5,"sent_bytes":625000,"achieved_mbps":1}',
        'NBPROBE_RESULT {"bytes":625000,"elapsed_s":5,"achieved_mbps":1,'
        '"integrity":"count-ok","origin":"entry-local"}',
    ])
    assert remote["origin"] == "entry-local" and remote["bytes"] == 625000
    connection = FakeConnection()
    original_connect = line_probe.deploy.connect
    old_username, old_password = os.environ.get("NB_SOCKS_USERNAME"), os.environ.get("NB_SOCKS_PASSWORD")
    try:
        line_probe.deploy.connect = lambda role: connection
        os.environ["NB_SOCKS_USERNAME"] = "probe-user"
        os.environ["NB_SOCKS_PASSWORD"] = "probe-password"
        remote = run_entry_local_probe("load", 1085, target_mbps=1, duration_s=5)
        request = json.loads(connection.stdin.value)
        assert request["socks_port"] == 1085 and request["duration_s"] == 5
        assert remote["origin"] == "entry-local" and connection.closed
        connection = FakeConnection()
        line_probe.deploy.connect = lambda role: connection
        remote = run_entry_local_probe("downlink", 1085, target_mbps=1, duration_s=5)
        request = json.loads(connection.stdin.value)
        assert request["mode"] == "downlink" and request["target_mbps"] == 1
        assert remote["origin"] == "entry-local" and connection.closed
    finally:
        line_probe.deploy.connect = original_connect
        if old_username is None:
            os.environ.pop("NB_SOCKS_USERNAME", None)
        else:
            os.environ["NB_SOCKS_USERNAME"] = old_username
        if old_password is None:
            os.environ.pop("NB_SOCKS_PASSWORD", None)
        else:
            os.environ["NB_SOCKS_PASSWORD"] = old_password
    probe_socket = FakeProbeSocket()
    original_socks_connect = line_probe.socks_connect
    try:
        line_probe.socks_connect = lambda *args, **kwargs: probe_socket
        cancelled = run_load_probe("127.0.0.1", 1085, 1, 5, stop_event=Stopped())
        assert cancelled["integrity"] == "cancelled"
        assert probe_socket.shutdown_how == line_probe.socket.SHUT_WR
    finally:
        line_probe.socks_connect = original_socks_connect
    original_connect = line_probe.deploy.connect
    original_run = line_probe.deploy.run
    original_log_path = line_probe.remote_log_path
    log_connections = []
    log_commands = []
    try:
        def connect_log(role):
            connection = FakeLogConnection()
            log_connections.append(connection)
            return connection

        def run_log(connection, command):
            log_commands.append(command)
            return "0\n" if "wc -c" in command else ""

        line_probe.deploy.connect = connect_log
        line_probe.deploy.run = run_log
        line_probe.remote_log_path = lambda connection, role: "/etc/NB/instances/00006_1/logs/missing log.log"
        assert line_probe.log_offset("entry") == 0
        assert line_probe.log_since("entry", 0) == ""
        assert all(connection.closed for connection in log_connections)
        assert all("test -f '/etc/NB/instances/00006_1/logs/missing log.log'" in command
                   for command in log_commands)
    finally:
        line_probe.deploy.connect = original_connect
        line_probe.deploy.run = original_run
        line_probe.remote_log_path = original_log_path
    ping = parse_ping("10 packets transmitted, 10 received, 0% packet loss\nrtt min/avg/max/mdev = 4.1/5.2/7.8/0.6 ms")
    assert ping["loss_pct"] == 0.0 and ping["rtt_avg_ms"] == 5.2

    line = ("linkq pool[0:0] mtu=1424 rtt=210.0ms(min180/max260) jit=20.0ms loss=0.20% "
            "dsent=2000 loss_raw=6 eff=4 spur=2 timer=0 repeat=6 retx=6/20 pre=0/0 "
            "reorder=163.0ms/24 tol=128/450.0ms ack=10ms cc=bbr:numeric(0)/1 "
            "cwin=120KB bif=30KB block=0/0/0 bw=10000Kbps sent=1000KB")
    summary = summarize_linkq(parse_linkq("\n".join([line] * 6)))
    assert summary["packets_observed"] == 12000
    assert summary["reorder_gap_max"] == 24
    assert summary["quic_ip_mtu_proven"] == 1452
    mtu_probe = {"status": "ok", "max_ip_mtu": 1500}
    candidate = recommend(summary, {"cc": "bbr", "mtu_max": 1280}, 10.0, mtu_probe)
    assert candidate["confidence"] == "load-qualified"
    assert candidate["reorder_gap"] == 32
    assert candidate["provisional"]["reorder_gap"] == 32
    assert candidate["reorder_delay_us"] == 120000
    assert candidate["auto_apply_allowed"] is False
    assert candidate["mtu_max"] == 1452
    assert candidate["mtu_evidence"]["confidence"] == "quic-and-df"

    normal = line.replace("reorder=163.0ms/24", "reorder=10.0ms/4")
    transient = line.replace("reorder=163.0ms/24", "reorder=900.0ms/512")
    robust = summarize_linkq(parse_linkq("\n".join([normal] * 6 + [transient])))
    assert robust["reorder_gap_max"] == 512
    assert robust["reorder_delay_max_ms"] == 900.0
    assert robust["reorder_gap_p95"] == 4
    assert robust["reorder_delay_p95_ms"] == 10.0

    clean = summarize_linkq(parse_linkq("\n".join([
        line.replace("rtt=210.0ms", "rtt=5.0ms")
            .replace("jit=20.0ms", "jit=0.2ms")
            .replace("reorder=163.0ms/24", "reorder=0.0ms/0")
    ] * 6)))
    protected = recommend(clean, {
        "cc": "cubic", "cwin_max_bytes": 524288, "mtu_max": 1452,
        "reorder_gap": 128, "reorder_delay_us": 450000,
    }, 10.0, mtu_probe)
    assert protected["cwin_max_bytes"] == 524288
    assert protected["reorder_gap"] == 8
    assert protected["reorder_delay_us"] == 20000

    idle = summarize_linkq([])
    current = {"cc": "bbr", "mtu_max": 1400,
               "reorder_gap": 128, "reorder_delay_us": 450000}
    idle_candidate = recommend(idle, current, 10.0)
    assert idle_candidate["confidence"] == "insufficient-load"
    assert idle_candidate["cc"] == "bbr"
    assert idle_candidate["reorder_gap"] == 128
    assert idle_candidate["mtu_max"] == 1400
    assert idle_candidate["auto_apply_allowed"] is False
    assert recommend_mtu(idle, None, current)["confidence"] == "unavailable-keep-current"
    admitted = evaluate_admission({"integrity": {"integrity": "ok"},
        "uplink": {"integrity": "count-ok", "achieved_mbps": 9.50},
        "downlink": {"integrity": "count-ok", "achieved_mbps": 9.50}}, 10.0)
    assert admitted["status"] == "admitted"
    assert admitted["required_ratio"] == 0.95
    assert admitted["duration_seconds"] == 90
    rejected = evaluate_admission({"integrity": {"integrity": "ok"},
        "uplink": {"integrity": "count-ok", "achieved_mbps": 9.50},
        "downlink": {"integrity": "count-ok", "achieved_mbps": 9.49}}, 10.0)
    assert rejected["status"] == "rejected"
    assert rejected["reasons"] == ["insufficient-downlink"]
    barrier = threading.Barrier(2)
    def direction(name):
        barrier.wait(timeout=1)
        return {"direction": name}
    uplink, downlink = run_probe_pair(lambda: direction("up"), lambda: direction("down"))
    assert uplink["direction"] == "up" and downlink["direction"] == "down"
    print("line_probe tests passed")


if __name__ == "__main__":
    main()
