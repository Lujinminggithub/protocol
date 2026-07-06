#!/usr/bin/env python3
"""查三跳实测内存 + BBR/FEC 协同状态。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    for role in ("egress", "relay", "ingress"):
        h = df.load_hosts()[role]
        cmd = r"""
set +e
P=$(pgrep -f '/etc/xgw/xgw run' | head -1)
echo -n "RSS/Data: "
grep -E 'VmRSS|VmData' /proc/$P/status 2>/dev/null | tr '\n' ' '
echo
echo -n "fec最近: "
tail -n 300 /etc/xgw/logs/$1.out.log 2>/dev/null | grep -aoE 'runtime.fec groups=[0-9]+ completed=[0-9]+ data_shards=[0-9]+ parity_shards=[0-9]+' | tail -1
echo -n "cc最近(srtt/loss/cwnd): "
tail -n 300 /etc/xgw/logs/$1.out.log 2>/dev/null | grep -aoE 'srtt_us=[0-9]+ min_rtt_us=[0-9]+ acked=[0-9]+ lost=[0-9]+' | tail -1
""".replace("$1", role)
        _, out, _ = df.run_remote(h, cmd, check=False, timeout=40)
        print(f"### {role} ({h.name}) ###\n{out}")


if __name__ == "__main__":
    main()
