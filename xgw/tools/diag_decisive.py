#!/usr/bin/env python3
"""决定性判据：TikTok first_byte=-1 流的 lifetime 分布（竞速 vs 真超时）+ ip.sb 对比基线 + egress TLS 中断率。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ing = df.load_hosts()["ingress"]
    fcmd = r"""
set +e; FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1
echo "=== TikTok first_byte=-1 流的 lifetime 分布 ==="
grep -a hy2front.flow.close $F 2>/dev/null | grep -a tiktok | grep -a 'first_byte_ms=-1' \
  | grep -aoE 'lifetime_ms=[0-9]+' | grep -oE '[0-9]+' \
  | awk '{if($1<1000)s++; else if($1<5000)m++; else if($1<31000)l++; else xl++} END{print "<1s(竞速嫌疑):"s+0"  1-5s:"m+0"  5-31s:"l+0"  >=31s(idle超时):"xl+0}'
echo "=== TikTok 拿到 first_byte 的 2 条是什么 target ==="
grep -a hy2front.flow.close $F 2>/dev/null | grep -a tiktok | grep -av 'first_byte_ms=-1' | grep -aoE 'target=[^ ]+ lifetime_ms=[0-9]+ open_ms=[0-9]+ first_packet_ms=[0-9-]+ first_byte_ms=[0-9-]+'
echo
echo "=== 对比基线：ip.sb / google 这些「正常」流的 close（有没有 first_byte）==="
grep -a hy2front.flow.close $F 2>/dev/null | grep -aE 'ip\.sb|google|baidu' | grep -aoE 'target=[^ ]+ lifetime_ms=[0-9]+ first_packet_ms=[0-9-]+ first_byte_ms=[0-9-]+' | tail -15
echo
echo "=== Apple 流（也走同一回程）的 first_byte 情况（对照）==="
grep -a hy2front.flow.close $F 2>/dev/null | grep -a apple | grep -aoE 'first_byte_ms=[0-9-]+' | grep -oE '\-?[0-9]+' | awk '{if($1<0)neg++; else ok++} END{print "apple first_byte: 有="ok+0" 无(-1)="neg+0}'
"""
    _, fout, _ = df.run_remote(ing, fcmd, check=False, timeout=60)
    print("##### FRONT #####\n" + fout)

    eg = df.load_hosts()["egress"]
    ecmd = r"""
set +e; L=/etc/xgw/logs/egress.out.log
echo "=== egress: TikTok 流 recv_fail vs recv_eof 比例（TLS 中断率）==="
echo -n "  target_recv_eof(正常完成): "; tail -n 50000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | grep -ac 'target_recv_eof'
echo -n "  target_recv_fail(中断): "; tail -n 50000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | grep -ac 'target_recv_fail'
echo "=== egress: recv_fail 流的 recv_bytes 分布（~600B=TLS握手后断）==="
tail -n 50000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | grep -a 'target_recv_fail' | grep -aoE 'recv_bytes=[0-9]+' | grep -oE '[0-9]+' | sort -n | awk '{a[NR]=$1} END{print "n="NR" min="a[1]" median="a[int(NR/2)]" max="a[NR]}'
echo "=== egress: TikTok 流 first_byte_ms（KZ上游响应速度，确认上游真的回了）==="
tail -n 50000 $L 2>/dev/null | grep -a 'tiktok.trace.egress.close' | grep -aoE 'first_byte_ms=[0-9-]+' | grep -oE '\-?[0-9]+' | awk '{if($1<0)neg++; else{ok++; if($1>mx)mx=$1}} END{print "有first_byte="ok+0" 无(-1)="neg+0" max_ms="mx+0}'
"""
    _, eout, _ = df.run_remote(eg, ecmd, check=False, timeout=60)
    print("\n##### EGRESS #####\n" + eout)


if __name__ == "__main__":
    main()
