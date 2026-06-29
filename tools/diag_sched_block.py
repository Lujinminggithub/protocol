#!/usr/bin/env python3
"""抓 runtime.sched.block 诊断：回程帧卡在哪个门控 + inflight/budget/ack_credit 值。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    for role in ("egress", "relay", "ingress"):
        h = df.load_hosts()[role]
        cmd = r"""
set +e; L=/etc/xgw/logs/%s.out.log
echo "=== sched.block 各 reason 命中次数 ==="
grep -aoE 'runtime.sched.block .*reason=[a-z_0-9]+' $L 2>/dev/null | grep -aoE 'reason=[a-z_0-9]+' | sort | uniq -c | sort -rn
echo "=== sched.block 最近 25 条原始（看 inflight/budget/ack_credit）==="
grep -a 'runtime.sched.block' $L 2>/dev/null | tail -25
echo "=== 该跳 stream_sched_drop / channel_not_ready（其它阻塞信号）==="
grep -aoE 'stream_sched_drop|channel_not_ready|return_queue_full' $L 2>/dev/null | sort | uniq -c
""" % role
        code, out, err = df.run_remote(h, cmd, check=False, timeout=60)
        print(f"\n########## {role} ({h.name}) ##########\n{out}")


if __name__ == "__main__":
    main()
