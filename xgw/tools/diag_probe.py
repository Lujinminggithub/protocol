#!/usr/bin/env python3
"""现网只读诊断：在不改动线上的前提下采集各跳关键状态。

复用 deploy_fix 的连接/跳板逻辑。仅执行只读命令（ss/ps/tail/grep），
不重启、不部署、不删改任何文件。
"""

from __future__ import annotations

import sys

import deploy_fix as df


def probe(role_filter=None):
    hosts = df.load_hosts()
    for role, host in hosts.items():
        if role_filter and role != role_filter:
            continue
        if role == "ingress":
            cmd = (
                "echo '=== listen ports (ss) ==='; "
                "ss -tunlp 2>/dev/null | grep -E ':20023|:19080|:51840' || echo '(none of 20023/19080/51840)'; "
                "echo '=== bridge ring ==='; ls -la /run/xgw/ 2>/dev/null; "
                "echo '=== front proc env/cmdline ==='; "
                "for p in $(pgrep -f xgw-edge-server); do echo \"pid=$p\"; tr '\\0' ' ' < /proc/$p/cmdline; echo; done; "
                "echo '=== front log present? ==='; ls -la /etc/xgw/logs/ 2>/dev/null; "
                "echo '=== ingress conf ==='; cat /etc/xgw/configs/ingress.conf 2>/dev/null; "
            )
        elif role == "relay":
            cmd = (
                "echo '=== relay conf ==='; cat /etc/xgw/configs/relay.conf 2>/dev/null; "
                "echo '=== listen ports ==='; ss -tunlp 2>/dev/null | grep -E ':51840' || echo '(no 51840)'; "
                "echo '=== relay.out.log tail 60 ==='; tail -n 60 /etc/xgw/logs/relay.out.log 2>/dev/null; "
                "echo '=== relay log: forward/select/close counts (last 5000 lines) ==='; "
                "tail -n 5000 /etc/xgw/logs/relay.out.log 2>/dev/null | grep -oE 'forward\\.select|close_from_egress|secure_open_fail|channel\\.select\\.not_ready|return_queue_full|proto=udp|proto=tcp' | sort | uniq -c; "
            )
        else:  # egress
            cmd = (
                "echo '=== egress conf ==='; cat /etc/xgw/configs/egress.conf 2>/dev/null; "
                "echo '=== listen ports ==='; ss -tunlp 2>/dev/null | grep -E ':51840' || echo '(no 51840)'; "
                "echo '=== egress logs dir ==='; ls -la /etc/xgw/logs/ 2>/dev/null; "
                "echo '=== egress.out.log tail 40 ==='; tail -n 40 /etc/xgw/logs/egress.out.log 2>/dev/null || echo '(no egress.out.log)'; "
            )
        code, out, err = df.run_remote(host, cmd, check=False, timeout=90)
        print(f"\n########## {role}  {host.name}  {host.host} ##########\n{out}\n{err}", flush=True)


if __name__ == "__main__":
    probe(sys.argv[1] if len(sys.argv) > 1 else None)
