#!/usr/bin/env python3
"""全链路 TikTok 故障抓取：前端 tiktok 请求/回程 + 三跳 srtt 延迟拆解。只读。"""
from __future__ import annotations
import deploy_fix as df


def front_probe(ingress):
    cmd = (
        "set +e; FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1; "
        "echo '=== front last_ts age ==='; "
        "LASTTS=$(tail -c 30000 $F 2>/dev/null | grep -aoE '\"ts\":[0-9]+' | tail -1 | grep -oE '[0-9]+'); "
        "NOW=$(date -u +%s); echo \"last_log_ts=$LASTTS now=$NOW age_sec=$((NOW-LASTTS))\"; "
        "echo; echo '=== ANY tiktok/byte/ttwstatic target reached front? (whole inode) ==='; "
        "grep -aoE '(tiktok[a-z0-9.-]*|ttwstatic|byteoversea|ibyteimg|pitaya-cl[a-z0-9.-]*|musical)[a-z0-9.-]*' $F 2>/dev/null | sort | uniq -c | sort -rn | head -40; "
        "echo '(empty => NO tiktok target ever hit 20023)'; "
        "echo; echo '=== distinct event.tcp/udp.request targets (whole inode) ==='; "
        "grep -aE 'event\\.(tcp|udp)\\.request' $F 2>/dev/null | grep -aoE 'target=[^ \"]+' | sort | uniq -c | sort -rn | head -50; "
        "echo; echo '=== flow.close reasons ==='; "
        "grep -aoE 'reason=[a-z_]+' $F 2>/dev/null | sort | uniq -c | sort -rn; "
        "echo; echo '=== first_packet elapsed_ms for non-apple (front delivery latency) ==='; "
        "grep -a 'flow.first_packet' $F 2>/dev/null | grep -avE 'apple|push|courier' | grep -aoE 'target=[^ ]+ elapsed_ms=[0-9-]+' | tail -25; "
        "echo; echo '=== first_packet elapsed_ms=-1 count (timeout, no first byte) ==='; "
        "grep -a 'flow.first_packet' $F 2>/dev/null | grep -c 'elapsed_ms=-1'; "
    )
    _, out, _ = df.run_remote(ingress, cmd, check=False, timeout=120)
    print(f"\n########## FRONT (gz-170) ##########\n{out}")


def hop_probe(host, role, logname):
    cmd = (
        "set +e; L=/etc/xgw/logs/" + logname + "; "
        "echo '=== " + role + " log size/age ==='; ls -la --time-style=full-iso $L 2>/dev/null; "
        "echo '=== srtt vs min_rtt (last cc lines, per peer) ==='; "
        "tail -n 4000 $L 2>/dev/null | grep -aoE 'peer=[0-9.]+:[0-9]+ .*srtt_us=[0-9]+ min_rtt_us=[0-9]+' "
        "| sed -E 's/.*peer=([0-9.]+):[0-9]+.*srtt_us=([0-9]+) min_rtt_us=([0-9]+)/\\1 \\2 \\3/' "
        "| awk '{cnt[$1]++; ss[$1]+=$2; mm[$1]+=$3} END{for(p in cnt) printf \"peer=%s n=%d avg_srtt_us=%d avg_minrtt_us=%d ratio=%.1f\\n\", p, cnt[p], ss[p]/cnt[p], mm[p]/cnt[p], (mm[p]>0?(ss[p]/cnt[p])/(mm[p]/cnt[p]):0)}'; "
        "echo '=== forward/proto/close/err counts (last 8000 lines) ==='; "
        "tail -n 8000 $L 2>/dev/null | grep -aoE 'forward\\.select|proto=udp|proto=tcp|close_from_egress|secure_open_fail|return_queue_full|target_connect_fail|target_recv_fail|target_recv_eof|bridge\\.egress\\.send' | sort | uniq -c; "
        "echo '=== egress落地: bridge.egress.send targets (last 20000) ==='; "
        "tail -n 20000 $L 2>/dev/null | grep -a 'bridge.egress.send' | grep -aoE 'target=[^ ]+' | sort | uniq -c | sort -rn | head -20; "
        "echo '=== any tiktok target landed at this hop? ==='; "
        "tail -n 20000 $L 2>/dev/null | grep -aoE '(tiktok[a-z0-9.-]*|byteoversea|ttwstatic)[a-z0-9.-]*' | sort | uniq -c | head; "
    )
    _, out, _ = df.run_remote(host, cmd, check=False, timeout=120)
    print(f"\n########## {role.upper()} ({host.name}) ##########\n{out}")


def main():
    hosts = df.load_hosts()
    front_probe(hosts["ingress"])
    hop_probe(hosts["ingress"], "ingress", "ingress.out.log")
    hop_probe(hosts["relay"], "relay", "relay.out.log")
    hop_probe(hosts["egress"], "egress", "egress.out.log")


if __name__ == "__main__":
    main()
