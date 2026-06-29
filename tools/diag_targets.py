#!/usr/bin/env python3
"""核查 ip.sb 流是否拿到回程，以及 tiktok 域名是否完全缺席。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ingress = df.load_hosts()["ingress"]
    cmd = (
        "set +e; "
        "FRONT=$(pgrep -f xgw-edge-server | head -1); "
        "F=/proc/$FRONT/fd/1; "
        "echo '=== ALL ip.sb related lines (full inode) ==='; "
        "grep -a 'ip.sb' $F 2>/dev/null | tail -40; "
        "echo; echo '=== ip.sb : did bridge_to_front bytes arrive? ==='; "
        "grep -a 'ip.sb' $F 2>/dev/null | grep -E 'bridge_to_front|first_packet|bridge.ok|flow.close|eof' | tail -20; "
        "echo; echo '=== ANY tiktok/ttwstatic/byte ever in inode? ==='; "
        "grep -aoE '(tiktok[a-z.]*|ttwstatic|byteoversea|ibyteimg|pitaya)[a-z0-9.-]*' $F 2>/dev/null | sort | uniq -c | sort -rn | head -30; "
        "echo '(if empty above: NO tiktok target ever reached the front)'; "
        "echo; echo '=== distinct targets in event.tcp.request (whole inode) ==='; "
        "grep -a 'event.tcp.request' $F 2>/dev/null | grep -oE 'target=[^ ]+' | sort | uniq -c | sort -rn | head -40; "
        "echo; echo '=== flow.close reasons (whole inode) ==='; "
        "grep -aoE 'reason=[a-z_]+' $F 2>/dev/null | sort | uniq -c | sort -rn; "
        "echo; echo '=== first_byte / first_packet stats for non-apple targets ==='; "
        "grep -a 'flow.first_packet' $F 2>/dev/null | grep -vE 'apple|push' | tail -15; "
    )
    code, out, err = df.run_remote(ingress, cmd, check=False, timeout=120)
    print(out)
    if err.strip():
        print("STDERR:", err[:1500])


if __name__ == "__main__":
    main()
