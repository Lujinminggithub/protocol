#!/usr/bin/env python3
"""逐跳 first_byte 对比：定位 TikTok 首字节延迟在哪一跳产生（egress上游 vs 隧道回程）。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    h = df.load_hosts()
    # egress：上游 first_byte（KZ→TikTok 响应速度）
    eg = h["egress"]
    ecmd = r"""
set +e; L=/etc/xgw/logs/egress.out.log
echo "=== egress: 最近 TikTok 流 upstream first_byte_ms 分布（KZ上游响应）==="
tail -n 4000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | grep -aoE 'first_byte_ms=[0-9-]+' | grep -oE '\-?[0-9]+' | awk '{if($1<0)neg++; else{n++; s+=$1; if($1>mx)mx=$1; if($1<500)fast++}} END{print "n="n+0" -1(无)="neg+0" 平均="(n?int(s/n):0)"ms max="mx+0" <500ms="fast+0}'
echo "=== egress: 最近10条 tiktok close（看 first_byte 与 send/recv）==="
tail -n 4000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | grep -aoE 'target=[^ ]+ lifetime_ms=[0-9]+ first_packet_ms=[0-9]+ first_byte_ms=[0-9]+' | tail -10
echo "=== egress: 回程队列满/背压 ==="
tail -n 4000 $L 2>/dev/null | grep -aoE 'egress.queue.full|return_queue_full' | sort | uniq -c
echo "=== egress: burst chunks/tick ==="
tail -n 4000 $L 2>/dev/null | grep -aoE 'bridge_egress.burst chunks=[0-9]+' | grep -oE '[0-9]+' | awk '{n++; s+=$1; if($1>mx)mx=$1} END{print "burst次数="n+0" 平均chunks="(n?s/n:0)" max="mx+0}'
"""
    _, eout, _ = df.run_remote(eg, ecmd, check=False, timeout=60)
    print("##### EGRESS（上游+回程出队）#####\n" + eout)

    # relay：回程转发延迟
    rl = h["relay"]
    rcmd = r"""
set +e; L=/etc/xgw/logs/relay.out.log
echo "=== relay: srtt/min_rtt（回程KZ→HK段）==="
tail -n 1000 $L 2>/dev/null | grep -a 'peer=2.135.147.71' | grep -aoE 'srtt_us=[0-9]+ min_rtt_us=[0-9]+' | tail -3
echo "=== relay: 回程转发 channel_not_ready / drop ==="
tail -n 4000 $L 2>/dev/null | grep -aoE 'channel_not_ready|stream_sched_drop|return_queue_full' | sort | uniq -c
echo "=== relay: tiktok first_byte（到HK）==="
tail -n 4000 $L 2>/dev/null | grep -a 'tiktok.trace.relay.first' | grep -aoE 'elapsed_ms=[0-9]+|first_byte_ms=[0-9]+' | grep -oE '[0-9]+' | awk '{n++;s+=$1;if($1>mx)mx=$1} END{print "n="n+0" avg="(n?int(s/n):0)"ms max="mx+0}'
"""
    _, rout, _ = df.run_remote(rl, rcmd, check=False, timeout=60)
    print("\n##### RELAY（回程转发）#####\n" + rout)


if __name__ == "__main__":
    main()
