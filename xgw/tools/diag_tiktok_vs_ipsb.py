#!/usr/bin/env python3
"""ip.sb 正常 vs TikTok 无网络 的对比：哪些关键域名缺席/失败/QUIC。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ing = df.load_hosts()["ingress"]
    cmd = r"""
set +e; FRONT=$(pgrep -f xgw-edge-server | head -1); F=/proc/$FRONT/fd/1
echo "=== 本轮所有 event.tcp/udp.request 的 target（含非tiktok，看全貌）==="
grep -aE 'event\.(tcp|udp)\.request' $F 2>/dev/null | grep -aoE 'target=[^ "]+' | sort | uniq -c | sort -rn | head -60
echo
echo "=== UDP（QUIC）请求：TikTok 现在大量用 QUIC/h3，UDP 是否进链路？==="
grep -a 'event.udp.request' $F 2>/dev/null | grep -aoE 'target=[^ "]+' | sort | uniq -c | sort -rn | head -30
echo "udp.open 总数:"; grep -ac 'udp.open.bridge.begin' $F 2>/dev/null
echo
echo "=== TikTok 关键登录域名是否出现 + 成败 ==="
for d in api-boot api-core-boot mssdk-boot tnc-boot vcs-boot log-boot frontier mon-boot webcast-boot api16-normal; do
  n=$(grep -ac "$d" $F 2>/dev/null)
  ok=$(grep -a "tcp.open.bridge.ok" $F 2>/dev/null | grep -ac "$d")
  echo "$d: lines=$n bridge.ok=$ok"
done
echo
echo "=== 有没有 bridge.begin 但从未 bridge.ok 的 tiktok 目标（连接建不起来）==="
grep -a 'tcp.open.bridge.begin' $F 2>/dev/null | grep -a tiktok | grep -aoE 'target=[^ ]+' | sort -u > /tmp/beg.txt
grep -a 'tcp.open.bridge.ok' $F 2>/dev/null | grep -a tiktok | grep -aoE 'target=[^ ]+' | sort -u > /tmp/ok.txt
echo "begin但未ok的目标:"; comm -23 /tmp/beg.txt /tmp/ok.txt | head -20
echo
echo "=== front 任何 error/timeout/reset/refused（全量）==="
grep -aiE 'error|timeout|refused|reset|deadline|fail|reject|denied' $F 2>/dev/null | grep -aiv 'flow.close\|err=<nil>' | tail -25
"""
    _, out, _ = df.run_remote(ing, cmd, check=False, timeout=90)
    print(out)


if __name__ == "__main__":
    main()
