#!/usr/bin/env python3
"""决定性判据：持续下载类 TikTok CDN 流的回程 chunk gap（下载流有2s+gap=回程卡顿铁证）。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ing = df.load_hosts()["ingress"]
    fcmd = r"""
set +e; FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1
echo "=== 持续下载流(pitayacdn/teko/pkgcdn CDN)的 bridge_to_front gap 分布 ==="
echo "（CDN下载数据本应连续，gap>=1s 即回程卡顿，非应用层间隔）"
grep -a direction=bridge_to_front $F 2>/dev/null | grep -aE 'pitayacdn|teko|pkgcdn|tiktokcdn' \
  | grep -aoE 'gap_ms=[0-9-]+' | grep -oE '\-?[0-9]+' \
  | awk '{if($1<0)neg++; else{tot++; if($1>=200&&$1<1000)s++; else if($1>=1000&&$1<3000)m++; else if($1>=3000)l++; if($1>mx)mx=$1}}
         END{print "首chunk(neg):"neg+0" 总chunk:"tot+0; print "  200ms-1s:"s+0"  1-3s(卡顿):"m+0"  >=3s(严重卡顿):"l+0"  max="mx+0"ms"}'
echo
echo "=== 单条 CDN 下载流的完整 chunk 序列（看连续数据里的 gap）==="
CONN=$(grep -a direction=bridge_to_front $F 2>/dev/null | grep -aE 'pitayacdn|teko' | grep -aoE 'conn=[0-9]+' | sort | uniq -c | sort -rn | head -1 | grep -oE '[0-9]+')
echo "选中 conn=$CONN（chunk最多的下载流）:"
grep -a "conn=$CONN " $F 2>/dev/null | grep -a direction=bridge_to_front | grep -aoE 'total_bytes=[0-9]+ chunks=[0-9]+ gap_ms=[0-9-]+' | tail -25
echo
echo "=== 对照：ip.sb/google 这类流有没有这种 gap（同回程路径）==="
grep -a direction=bridge_to_front $F 2>/dev/null | grep -aE 'ip\.sb|google|gstatic|baidu' | grep -aoE 'gap_ms=[0-9-]+' | grep -oE '\-?[0-9]+' | awk '{if($1>=1000)slow++; tot++} END{print "非tiktok流: 总chunk="tot+0" gap>=1s="slow+0}'
"""
    _, fout, _ = df.run_remote(ing, fcmd, check=False, timeout=60)
    print("##### FRONT 回程卡顿判据 #####\n" + fout)


if __name__ == "__main__":
    main()
