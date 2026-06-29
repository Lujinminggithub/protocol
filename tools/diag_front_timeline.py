#!/usr/bin/env python3
"""看前端刚写入的增量内容 + 最近时间线，判断是否只有旧连接 keepalive、无新 stream。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ingress = df.load_hosts()["ingress"]
    cmd = (
        "set +e; "
        "FRONT=$(pgrep -f xgw-edge-server | head -1); "
        "echo '=== absolute last 12 log lines (real tail of deleted inode) ==='; "
        "tail -c 6000 /proc/$FRONT/fd/1 2>/dev/null | tail -12; "
        "echo; echo '=== last ts in log vs now ==='; "
        "LASTTS=$(tail -c 20000 /proc/$FRONT/fd/1 2>/dev/null | grep -oE '\"ts\":[0-9]+' | tail -1 | grep -oE '[0-9]+'); "
        "NOW=$(date -u +%s); echo \"last_log_ts=$LASTTS now=$NOW age_sec=$((NOW-LASTTS))\"; "
        "echo; echo '=== distinct remote clients seen in last 300k ==='; "
        "tail -c 300000 /proc/$FRONT/fd/1 2>/dev/null | grep -oE 'remote=[0-9.]+:[0-9]+' | sort | uniq -c | sort -rn | head; "
        "echo; echo '=== distinct hy2_conn ids in last 300k ==='; "
        "tail -c 300000 /proc/$FRONT/fd/1 2>/dev/null | grep -oE 'hy2_conn=[0-9]+' | sort | uniq -c | sort -rn | head; "
        "echo; echo '=== count event.tcp.request / udp.request in WHOLE log inode ==='; "
        "grep -aoE 'event\\.(tcp|udp)\\.request' /proc/$FRONT/fd/1 2>/dev/null | sort | uniq -c; "
        "echo; echo '=== last 3 event.tcp.request lines (with ts) ==='; "
        "grep -a 'event.tcp.request' /proc/$FRONT/fd/1 2>/dev/null | tail -3; "
        "echo; echo '=== 20023 live capture: show actual src IPs hitting it (10s) ==='; "
        "if command -v tcpdump >/dev/null 2>&1; then "
        "  timeout 10 tcpdump -ni any 'udp port 20023' -c 60 2>/dev/null | "
        "    grep -oE 'IP6? [0-9a-f.:]+ >' | sort | uniq -c | sort -rn | head; "
        "fi; "
        "echo '=== now ==='; date -u +%FT%TZ; "
    )
    code, out, err = df.run_remote(ingress, cmd, check=False, timeout=120)
    print(out)
    if err.strip():
        print("STDERR:", err[:1500])


if __name__ == "__main__":
    main()
