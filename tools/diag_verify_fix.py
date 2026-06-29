#!/usr/bin/env python3
"""修复后验证：前端 TikTok 首包/close/gap + burst chunks/tick。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ing = df.load_hosts()["ingress"]
    cmd = r"""
set +e
FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1
echo "=== front last_ts age ==="
LAST=$(tail -c 30000 $F 2>/dev/null | grep -aoE '"ts":[0-9]+' | tail -1 | grep -oE '[0-9]+')
NOW=$(date -u +%s); echo "age_sec=$((NOW-LAST))"
echo "=== front tiktok flow.first_packet elapsed_ms (交付App) ==="
grep -a flow.first_packet $F 2>/dev/null | grep -a tiktok | grep -aoE 'target=[^ ]+ elapsed_ms=[0-9-]+' | tail -25
echo "=== front tiktok elapsed_ms=-1 (首字节超时) count ==="
grep -a flow.first_packet $F 2>/dev/null | grep -a tiktok | grep -c 'elapsed_ms=-1'
echo "=== front flow.close reasons (tiktok) ==="
grep -a flow.close $F 2>/dev/null | grep -a tiktok | grep -aoE 'reason=[a-z_]+' | sort | uniq -c
echo "=== front bridge_to_front gap_ms (tiktok回程chunk间隔) max + 分布 ==="
grep -a direction=bridge_to_front $F 2>/dev/null | grep -a tiktok | grep -aoE 'gap_ms=[0-9-]+' | grep -oE '\-?[0-9]+' | awk '{if($1<0){neg++}else{if($1>mx)mx=$1; if($1>=2000)slow++; tot++}} END{print "neg(first):"neg+0" tot:"tot+0" gap>=2s:"slow+0" max_gap_ms:"mx+0}'
echo "=== front: full lifecycle 最近几条 tiktok api-boot ==="
grep -a 'api-boot.tiktokv.com' $F 2>/dev/null | grep -aE 'flow.open|first_packet|flow.close|bridge.ok' | tail -12
echo "=== runtime.bridge_egress.burst chunks (验证 >1) ==="
tail -n 80000 /etc/xgw/logs/ingress.out.log 2>/dev/null | grep -aoE 'bridge_egress.burst chunks=[0-9]+' | sort | uniq -c | sort -rn | head
echo "=== egress side: tiktok close reasons (本轮) ==="
"""
    code, out, err = df.run_remote(ing, cmd, check=False, timeout=90)
    print(out)

    eg = df.load_hosts()["egress"]
    ecmd = r"""
set +e; L=/etc/xgw/logs/egress.out.log
echo "=== egress tiktok close reasons ==="
tail -n 40000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | grep -aoE 'reason=[a-z_]+' | sort | uniq -c
echo "=== egress target_recv_fail / connect_fail 本轮 ==="
tail -n 40000 $L 2>/dev/null | grep -aoE 'target_recv_fail|target_connect_fail|return_queue_full|target_recv_eof' | sort | uniq -c
echo "=== egress: 哪些 tiktok 目标 queue.full（大响应体）==="
tail -n 40000 $L 2>/dev/null | grep -a 'bridge.egress.queue.full' | grep -aoE 'target=[^ ]+' | sort | uniq -c | sort -rn | head
"""
    _, eout, _ = df.run_remote(eg, ecmd, check=False, timeout=90)
    print("\n########## EGRESS ##########\n" + eout)


if __name__ == "__main__":
    main()
