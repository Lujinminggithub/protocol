#!/usr/bin/env python3
from line_probe import parse_linkq, parse_ping, recommend, recommend_mtu, summarize_linkq


def main() -> None:
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
    print("line_probe tests passed")


if __name__ == "__main__":
    main()
