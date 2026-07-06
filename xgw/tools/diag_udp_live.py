#!/usr/bin/env python3
"""实时抓 20023 包 + 前端 UDP 日志增量，判断 TikTok 测试时 UDP 是否进入前端。只读。"""
from __future__ import annotations
import time
import deploy_fix as df


def run_once():
    ing = df.load_hosts()["ingress"]
    cmd = r"""
set +e
FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1
U1=$(grep -ac 'udp.open.bridge.begin' $F 2>/dev/null)
echo "=== 抓 20023 包 15s（分 TCP/UDP 不可分，QUIC=UDP；按包数/字节）==="
echo "提示：QUIC 初始包通常 >1200B；hysteria 控制/数据也在 UDP 20023"
timeout 15 tcpdump -ni any 'udp port 20023' -ttq 2>/dev/null | \
  awk '{n++; if(match($0,/length [0-9]+/)){l=substr($0,RSTART+7,RLENGTH-7); tot+=l; if(l>1000)big++}}
       END{print "udp20023_pkts="n+0" bytes="tot+0" big(>1000B)="big+0}'
U2=$(grep -ac 'udp.open.bridge.begin' $F 2>/dev/null)
echo "=== 这15s内前端新增 udp.open.bridge.begin: $((U2-U1)) ==="
echo "=== 最近 30 条 udp.* 事件 target ==="
tail -c 200000 $F 2>/dev/null | grep -aoE 'udp\.(open\.bridge\.begin|classify|bridge\.session|open\.bridge_dial\.fail)[^"]*target=[^ "]+' | grep -aoE 'target=[^ "]+' | sort | uniq -c | sort -rn | head -20
echo "=== udp dial fail（UDP落地失败）==="
grep -a 'udp.open.bridge_dial.fail' $F 2>/dev/null | tail -10
echo "(空=无UDP落地失败)"
echo "=== now ==="; date -u +%FT%TZ
"""
    code, out, err = df.run_remote(ing, cmd, check=False, timeout=60)
    return out, err


def main():
    for attempt in range(4):
        try:
            out, err = run_once()
            if out.strip():
                print(out)
                if err.strip():
                    print("STDERR:", err[:500])
                return
        except Exception as e:
            print(f"[retry {attempt+1}] {e}")
            time.sleep(3)


if __name__ == "__main__":
    main()
