#!/usr/bin/env python3
"""egress 落地质量深挖：TikTok 上游 connect 成败 / recv_fail 原因 / 出口IP。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    h = df.load_hosts()
    egress = h["egress"]
    cmd = (
        "set +e; L=/etc/xgw/logs/egress.out.log; "
        "echo '=== egress out IP (KZ public) ==='; "
        "command -v curl >/dev/null && timeout 8 curl -s https://ip.sb 2>/dev/null || echo '(no curl/blocked)'; "
        "echo; echo '=== egress: distinct log msg kinds (last 30000) ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -aoE '\\b(bridge\\.egress\\.[a-z]+|runtime\\.[a-z.]+|target_[a-z_]+|tiktok\\.trace\\.egress\\.[a-z]+)\\b' | sort | uniq -c | sort -rn | head -40; "
        "echo; echo '=== target_recv_fail context (what errno/target) ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -a 'target_recv_fail' | tail -15; "
        "echo; echo '=== target_connect_fail (any tiktok connect failures?) ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -aE 'connect_fail|connect.*err|EOF|refused|timeout|reset' | grep -aiE 'tiktok|byte|fail|refus|reset|timeout' | tail -20; "
        "echo; echo '=== tiktok.trace.egress.open vs .first vs .close (matched?) ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -aoE 'tiktok\\.trace\\.egress\\.[a-z]+' | sort | uniq -c; "
        "echo; echo '=== sample full egress.first / egress.close tiktok lines ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | tail -8; "
    )
    _, out, _ = df.run_remote(egress, cmd, check=False, timeout=120)
    print(f"\n########## EGRESS DEEP (kz-1) ##########\n{out}")

    ingress = h["ingress"]
    fcmd = (
        "set +e; FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1; "
        "echo '=== front: full lifecycle of a few tiktok api-boot flows ==='; "
        "grep -a 'api-boot.tiktokv.com' $F 2>/dev/null | grep -aE 'flow.open|first_packet|flow.close|bridge.ok|bridge_to_front|front_to_bridge.eof' | tail -30; "
        "echo; echo '=== front: bridge_to_front (回程是否真到App) chunk count per tiktok target ==='; "
        "grep -a 'direction=bridge_to_front' $F 2>/dev/null | grep -aoE 'target=[^ ]*tiktok[^ ]*' | sort | uniq -c | sort -rn | head -20; "
        "echo '(if empty: 回程数据没有 bridge->front，App 收不到 = 无网络根因)'; "
        "echo; echo '=== front: front_to_bridge but NO bridge_to_front targets (单向/回程缺失) ==='; "
        "echo 'sent(f2b) targets:'; grep -a 'direction=front_to_bridge' $F 2>/dev/null | grep -aoE 'target=[^ ]*tiktok[^ ]*' | sort -u | head -40 > /tmp/f2b.txt; wc -l < /tmp/f2b.txt; "
        "echo 'recv(b2f) targets:'; grep -a 'direction=bridge_to_front' $F 2>/dev/null | grep -aoE 'target=[^ ]*tiktok[^ ]*' | sort -u | head -40 > /tmp/b2f.txt; wc -l < /tmp/b2f.txt; "
        "echo 'targets that SENT but never RECEIVED (no return):'; comm -23 /tmp/f2b.txt /tmp/b2f.txt | head -30; "
    )
    _, out2, _ = df.run_remote(ingress, fcmd, check=False, timeout=120)
    print(f"\n########## FRONT RETURN-PATH (gz-170) ##########\n{out2}")


if __name__ == "__main__":
    main()
