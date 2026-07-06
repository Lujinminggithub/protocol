#!/usr/bin/env python3
"""双向逐跳延迟拆解：去程(client→上游)first_packet vs 回程(上游→client)first_byte，定位协议层延迟在哪一段。只读。"""
from __future__ import annotations
import deploy_fix as df


def hop(role, log):
    h = df.load_hosts()[role]
    cmd = r"""
set +e; L=/etc/xgw/logs/%s.out.log
echo "--- %s phase 各类(去程first_packet=收到客户端请求, 回程first_byte=上游响应到本跳) ---"
# 去程: 该跳accept请求到first_packet(收到客户端数据)
tail -n 12000 $L 2>/dev/null | grep -a "tiktok.trace.%s" | grep -aoE 'first_packet_ms=[0-9]+' | grep -oE '[0-9]+' | sort -n | awk '{a[NR]=$1}END{if(NR)print "去程first_packet_ms: n="NR" p50="a[int(NR/2)]" max="a[NR]}'
# 回程: 该跳收到上游响应first_byte
tail -n 12000 $L 2>/dev/null | grep -a "tiktok.trace.%s" | grep -aoE 'first_byte_ms=[0-9]+' | grep -oE '[0-9]+' | sort -n | awk '{a[NR]=$1}END{if(NR)print "回程first_byte_ms: n="NR" p50="a[int(NR/2)]" max="a[NR]}'
# 该跳 srtt vs min_rtt(bufferbloat)
echo "srtt/minrtt(各peer):"
tail -n 300 $L 2>/dev/null | grep -aoE 'peer=[0-9.]+:[0-9]+ .*srtt_us=[0-9]+ min_rtt_us=[0-9]+' | sed -E 's/.*peer=([0-9.]+):[0-9]+.*srtt_us=([0-9]+) min_rtt_us=([0-9]+)/  \1 srtt=\2us minrtt=\3us/' | sort -u | tail -4
""" % (log, role, role, role)
    _, out, _ = df.run_remote(h, cmd, check=False, timeout=60)
    print(f"\n######## {role} ({h.name}) ########\n{out}")


def main():
    for role in ("ingress", "relay", "egress"):
        hop(role, role)
    # egress: 上游连接建立耗时(去程到达上游) + 上游first_byte(纯上游响应)
    eg = df.load_hosts()["egress"]
    cmd = r"""
set +e; L=/etc/xgw/logs/egress.out.log
echo "=== egress: 上游(KZ→TikTok)的纯连接/响应延迟 ==="
tail -n 12000 $L 2>/dev/null | grep -a 'egress.phase.close' | grep -a tiktok | grep -aoE 'first_packet_ms=[0-9]+ first_byte_ms=[0-9]+' | grep -oE 'first_byte_ms=[0-9]+' | grep -oE '[0-9]+' | sort -n | awk '{a[NR]=$1}END{if(NR)print "egress上游first_byte_ms(KZ→TikTok纯往返): n="NR" p50="a[int(NR/2)]" p90="a[int(NR*0.9)]" max="a[NR]}'
tail -n 12000 $L 2>/dev/null | grep -a 'egress.phase.open' | grep -a tiktok | grep -aoE 'connect_ms=[0-9]+' | grep -oE '[0-9]+' | sort -n | awk '{a[NR]=$1}END{if(NR)print "egress上游connect_ms: n="NR" p50="a[int(NR/2)]" max="a[NR]}'
"""
    _, out, _ = df.run_remote(eg, cmd, check=False, timeout=60)
    print(out)


if __name__ == "__main__":
    main()
