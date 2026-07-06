#!/usr/bin/env python3
"""定位 burst 日志真实节点 + 120s gap 的真实来源（是卡顿还是长连接空闲）。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    hosts = df.load_hosts()
    # burst 日志在哪个节点？egress 才走 bridge_egress 回程
    for role in ("egress", "relay", "ingress"):
        h = hosts[role]
        cmd = (
            "set +e; L=/etc/xgw/logs/%s.out.log; "
            "echo '=== %s: bridge_egress.burst 日志 ==='; "
            "grep -aoE 'bridge_egress.burst chunks=[0-9]+' $L 2>/dev/null | grep -oE 'chunks=[0-9]+' | sort | uniq -c | sort -rn | head; "
            "echo '(空=该节点无burst日志)'; "
            "echo '=== %s: 是否在跑新二进制(burst符号)? ==='; "
            "grep -ac 'bridge_egress.burst\\|egress.queue.full' $L 2>/dev/null; "
        ) % (role, role, role)
        _, out, _ = df.run_remote(h, cmd, check=False, timeout=60)
        print(f"\n##### {role} ({h.name}) #####\n{out}")

    # 前端：这些 gap>=2s 的 chunk 是哪些 target？是长连接keepalive还是bootstrap流？
    ing = hosts["ingress"]
    fcmd = r"""
set +e; FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1
echo "=== 前端 gap>=2s 的 chunk: 按 target 分类（区分长连接 vs bootstrap）==="
grep -a direction=bridge_to_front $F 2>/dev/null | grep -a tiktok | awk '
  match($0, /gap_ms=[0-9-]+/){g=substr($0,RSTART+7,RLENGTH-7)+0}
  match($0, /target=[^ ]+/){t=substr($0,RSTART+7,RLENGTH-7)}
  g>=2000 {cnt[t]++}
  END{for(x in cnt) print cnt[x], x}' | sort -rn | head -25
echo "=== 这些大gap是不是发生在流的末尾(chunks高/total高)? 看几条原始行 ==="
grep -a direction=bridge_to_front $F 2>/dev/null | grep -a tiktok | grep -aE 'gap_ms=(1[0-9]{4,}|[2-9][0-9]{3})' | tail -15
"""
    _, fout, _ = df.run_remote(ing, fcmd, check=False, timeout=90)
    print(f"\n##### FRONT gap analysis #####\n{fout}")


if __name__ == "__main__":
    main()
