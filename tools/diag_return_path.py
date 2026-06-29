#!/usr/bin/env python3
"""定位回程 60s 滞留：relay/ingress 回程队列、流生命周期、close 传播延迟。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    h = df.load_hosts()
    # egress: 这些 60s lifetime 流，真实回程在 egress 侧是几秒内发完的吗？
    egress = h["egress"]
    ecmd = (
        "set +e; L=/etc/xgw/logs/egress.out.log; "
        "echo '=== egress: api-boot 流 send/recv 的实际耗时分布 (first_byte_ms vs lifetime_ms) ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | grep -aoE 'first_byte_ms=[0-9-]+ .*lifetime_ms=[0-9]+|lifetime_ms=[0-9]+ first_packet_ms=[0-9]+ first_byte_ms=[0-9]+' | head -20; "
        "echo '=== egress: lifetime_ms 直方 (是不是都卡在 ~60000?) ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -a 'egress.phase.close' | grep -aoE 'lifetime_ms=[0-9]+' | grep -oE '[0-9]+' "
        "| awk '{b=int($1/10000)*10; h[b]++} END{for(x=0;x<=120;x+=10) printf \"%3ds-%3ds: %d\\n\", x, x+10, h[x*1000]+0}'; "
        "echo '=== egress: return queue / drain backpressure signals ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -aoE 'return_queue_full|queue.*full|drain|backpress|egress.queue[a-z. ]*' | sort | uniq -c | head; "
        "echo '=== egress: bridge.egress.queue full lines ==='; "
        "tail -n 30000 $L 2>/dev/null | grep -a 'bridge.egress.queue' | tail -10; "
    )
    _, out, _ = df.run_remote(egress, ecmd, check=False, timeout=120)
    print(f"\n########## EGRESS lifetime/queue (kz-1) ##########\n{out}")

    # relay: 回程队列、close 传播
    relay = h["relay"]
    rcmd = (
        "set +e; L=/etc/xgw/logs/relay.out.log; "
        "echo '=== relay: return-path / queue / drain msg kinds (last 20000) ==='; "
        "tail -n 20000 $L 2>/dev/null | grep -aoE '\\b(runtime\\.[a-z.]+|return[_a-z]*|queue[_a-z]*|drain[_a-z]*|forward\\.[a-z]+|close[_a-z]*)\\b' | sort | uniq -c | sort -rn | head -30; "
        "echo '=== relay: outstanding/inflight on the gz return peer (172.17.7.105) ==='; "
        "tail -n 2000 $L 2>/dev/null | grep -a 'peer=172.17.7.105' | grep -aoE 'outstanding=[0-9]+ |inflight=[0-9]+|cwnd=[0-9]+|pacing_bps=[0-9]+' | tail -20; "
        "echo '=== relay: srtt spikes on return peer (172.17.7.105) max ==='; "
        "tail -n 4000 $L 2>/dev/null | grep -a 'peer=172.17.7.105' | grep -aoE 'srtt_us=[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -5; "
    )
    _, out2, _ = df.run_remote(relay, rcmd, check=False, timeout=120)
    print(f"\n########## RELAY return-path (hk-176) ##########\n{out2}")

    # front: 这些 gap_ms=60000 的 chunk，是不是回程的最后一个 chunk（close_notify 尾包）
    ingress = h["ingress"]
    fcmd = (
        "set +e; FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1; "
        "echo '=== front: bridge_to_front gap_ms 分布 (回程chunk间隔) ==='; "
        "grep -a 'direction=bridge_to_front' $F 2>/dev/null | grep -aoE 'gap_ms=[0-9-]+' | grep -oE '\\-?[0-9]+' "
        "| awk '{if($1<0){neg++}else{b=int($1/5000)*5; h[b]++; if($1>mx)mx=$1}} END{print \"gap<0(first):\"neg+0; for(x=0;x<=65;x+=5) if(h[x*1000]) printf \"%2ds-%2ds: %d\\n\",x,x+5,h[x*1000]; print \"max_gap_ms=\"mx}'; "
        "echo '=== front: chunk_bytes=24 (tiny tail/close_notify) 的占比 ==='; "
        "echo -n 'b2f chunk_bytes=24 count: '; grep -a 'direction=bridge_to_front' $F 2>/dev/null | grep -c 'chunk_bytes=24'; "
        "echo -n 'b2f total chunk lines: '; grep -a 'direction=bridge_to_front' $F 2>/dev/null | grep -c 'chunk_bytes='; "
        "echo '=== front: flow.close lifetime for tiktok (App 端看到的流时长) ==='; "
        "grep -a 'flow.close' $F 2>/dev/null | grep -a tiktok | grep -aoE 'target=[^ ]+ .*lifetime_ms=[0-9]+|lifetime_ms=[0-9]+' | tail -15; "
    )
    _, out3, _ = df.run_remote(ingress, fcmd, check=False, timeout=120)
    print(f"\n########## FRONT gap/tail (gz-170) ##########\n{out3}")


if __name__ == "__main__":
    main()
