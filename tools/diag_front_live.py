#!/usr/bin/env python3
"""判断前端是否在实时处理新连接：两次采样 deleted-log 字节偏移 + 抓 20023 实时包计数。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ingress = df.load_hosts()["ingress"]
    cmd = (
        "set +e; "
        "FRONT=$(pgrep -f xgw-edge-server | head -1); "
        "CING=$(pgrep -f '/etc/xgw/xgw run /etc/xgw/configs/ingress.conf' | head -1); "
        # 前端日志当前总字节（fd/1 指向 deleted inode），间隔 8s 两采
        "sz1=$(wc -c < /proc/$FRONT/fd/1 2>/dev/null); "
        "cin1=$(wc -c < /proc/$CING/fd/1 2>/dev/null); "
        # 同时统计 20023 实时收包：8 秒窗口
        "echo '=== 20023 packet capture 8s (tcpdump if present) ==='; "
        "if command -v tcpdump >/dev/null 2>&1; then "
        "  timeout 8 tcpdump -ni any 'udp port 20023' -c 200 2>/dev/null | "
        "    awk '{n++} END{print \"udp20023_pkts=\"n+0}'; "
        "else echo 'no tcpdump; using /proc/net/udp snapshot'; fi; "
        "sleep 1; "
        "sz2=$(wc -c < /proc/$FRONT/fd/1 2>/dev/null); "
        "cin2=$(wc -c < /proc/$CING/fd/1 2>/dev/null); "
        "echo \"front_log_bytes: $sz1 -> $sz2 (delta=$((sz2-sz1)))\"; "
        "echo \"cing_log_bytes:  $cin1 -> $cin2 (delta=$((cin2-cin1)))\"; "
        # 前端最后一条真实 tcp.request 的时间戳
        "echo '=== last real front events ==='; "
        "tail -c 200000 /proc/$FRONT/fd/1 2>/dev/null | grep -oE 'event\\.(tcp|udp)\\.request|hy2front\\.flow\\.open|udp\\.open' | tail -5; "
        "echo '=== front: any auth/handshake/quic errors in last 200k ==='; "
        "tail -c 400000 /proc/$FRONT/fd/1 2>/dev/null | grep -iE 'error|fail|reject|handshake|auth|tls|alpn|deadline|refus|panic|fatal' | tail -30; "
        "echo '=== front conn socket stats: rx queue on 20023 ==='; "
        "ss -uanp 2>/dev/null | grep ':20023'; "
        "echo '=== /proc/net/udp recvq for 20023 (hex port=4E27) ==='; "
        "awk 'NR==1 || $2 ~ /:4E27$/ {print}' /proc/net/udp 2>/dev/null; "
        "echo '=== current UTC ==='; date -u +%FT%TZ; "
    )
    code, out, err = df.run_remote(ingress, cmd, check=False, timeout=120)
    print(out)
    if err.strip():
        print("STDERR:", err[:2000])


if __name__ == "__main__":
    main()
