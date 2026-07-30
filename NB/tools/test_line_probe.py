#!/usr/bin/env python3
import json
import os

import line_probe
from line_probe import (ENTRY_LOCAL_PROBE_SCRIPT, evaluate_admission, fnv1a64,
                        parse_entry_probe_output, parse_linkq, parse_ping, recommend, recommend_mtu,
                        run_entry_local_probe, summarize_linkq)


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


def main() -> None:
    compile(ENTRY_LOCAL_PROBE_SCRIPT, "<entry-local-probe>", "exec")
    assert fnv1a64(b"abc") == 0xe71fa2190541574b
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
    assert candidate["reorder_gap"] == 30
    assert candidate["provisional"]["reorder_gap"] == 30
    assert candidate["auto_apply_allowed"] is False
    assert candidate["mtu_max"] == 1452
    assert candidate["mtu_evidence"]["confidence"] == "quic-and-df"

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
    assert protected["reorder_gap"] == 128
    assert protected["reorder_delay_us"] == 450000

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
        "load": {"integrity": "count-ok", "achieved_mbps": 9.2}}, 10.0)
    assert admitted["status"] == "admitted"
    rejected = evaluate_admission({"integrity": {"integrity": "ok"},
        "load": {"integrity": "count-ok", "achieved_mbps": 8.9}}, 10.0)
    assert rejected["status"] == "rejected"
    print("line_probe tests passed")


if __name__ == "__main__":
    main()
