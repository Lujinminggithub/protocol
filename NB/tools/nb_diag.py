#!/usr/bin/env python3
"""NB 雪崩诊断 —— 复用 deploy.py 的连接逻辑, 抓三跳运行时证据。

用法:
  python tools/nb_diag.py probe          # 只读: 拉当前三跳日志统计 + entry 连接状态(不重启)
  python tools/nb_diag.py stress [N]     # 受控并发压测 N(默认20) + 三跳日志前后差量归因
"""
from __future__ import annotations
import sys, time
import deploy  # 复用 connect/run/_role_host/WORK

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

WORK = deploy.WORK
LOG = lambda role: f"{WORK}/logs/nb-{role}.log"
SOCKS_PORT = deploy.DEFAULT_SOCKS_PORT
SOCKS_ARG = f"--socks5-hostname 127.0.0.1:{SOCKS_PORT}"


def _stat_block(c, role):
    lg = LOG(role)
    return deploy.run(c,
        f"echo '== {role} =='; "
        f"pgrep -x nb_node>/dev/null && echo PROC=ALIVE || echo PROC=DEAD; "
        f"echo LOG_LINES=$(wc -l <{lg} 2>/dev/null); "
        f"echo ERR=$(grep -c ' ERROR ' {lg} 2>/dev/null); "
        f"echo WARN=$(grep -c ' WARN ' {lg} 2>/dev/null); "
        f"echo CONNECT=$(grep -c 'socks CONNECT' {lg} 2>/dev/null); "
        f"echo NEXTHOP=$(grep -c 'nexthop' {lg} 2>/dev/null); "
        f"echo TARGET=$(grep -c '\\-> target' {lg} 2>/dev/null); "
        f"echo CONNFAIL=$(grep -c 'connect fail\\|connect target.*fail' {lg} 2>/dev/null); "
        f"echo PSFREE=$(grep -c 'ps free' {lg} 2>/dev/null); ")


def probe():
    for r in ("entry", "middle", "exit"):
        c = deploy.connect(r)
        out = _stat_block(c, r)
        if r == "entry":
            out += deploy.run(c,
                f"echo SS_{SOCKS_PORT}_ESTAB=$(ss -tn state established 2>/dev/null|grep -c :{SOCKS_PORT}); "
                f"echo SS_{SOCKS_PORT}_ALL=$(ss -tan 2>/dev/null|grep -c :{SOCKS_PORT}); ")
        if r == "middle":
            out += deploy.run(c,
                f"echo FEC_ENV=$(systemctl show -p Environment --value {deploy._service_name('middle')} 2>/dev/null); ")
        out += deploy.run(c, f"echo '--- tail 20 ---'; tail -20 {LOG(r)} 2>/dev/null")
        print(out)
        # 关键: 抓 WARN/ERROR 全量(雪崩根因多半在此)
        print(deploy.run(c, f"echo '--- {r} WARN/ERROR (last 40) ---'; grep -E ' (WARN|ERROR) ' {LOG(r)} 2>/dev/null|tail -40"))
        c.close()


def _counts(c, role):
    import re
    raw = _stat_block(c, role)
    d = {}
    for line in raw.splitlines():
        if "=" in line and line.split("=")[0] in ("ERR","WARN","CONNECT","NEXTHOP","TARGET","CONNFAIL","PSFREE","LOG_LINES"):
            k,v = line.split("=",1); d[k]=int(v) if v.strip().isdigit() else 0
    return d


def stress(n=20):
    conns = {r: deploy.connect(r) for r in ("entry","middle","exit")}
    print(f"=== 压测前基线 (N={n}) ===")
    before = {r: _counts(conns[r], r) for r in conns}
    for r in conns: print(r, before[r])
    # entry 上并发压测: 逐个记录 http_code
    print(f"\n=== 并发 {n} 个 HTTPS ===")
    cmd = (f"for i in $(seq 1 {n}); do "
           f"curl -s -o /dev/null -w '%{{http_code}}\\n' {SOCKS_ARG} https://www.google.com/robots.txt --max-time 20 & done; "
           f"wait; echo; echo SS_ESTAB=$(ss -tn state established 2>/dev/null|grep -c :{SOCKS_PORT})")
    print(deploy.run(conns["entry"], cmd, tmo=60))
    time.sleep(2)
    print("=== 压测后差量归因 ===")
    for r in conns:
        after = _counts(conns[r], r)
        delta = {k: after.get(k,0)-before[r].get(k,0) for k in after}
        print(r, "Δ", {k:v for k,v in delta.items() if v})
    # 三跳新增 WARN/ERROR
    for r in conns:
        print(deploy.run(conns[r], f"echo '--- {r} 新增 WARN/ERROR ---'; tail -30 {LOG(r)} 2>/dev/null|grep -E ' (WARN|ERROR) '"))
    for c in conns.values(): c.close()


def _one_round(c, n, tag):
    cmd = (f"for i in $(seq 1 {n}); do "
           f"curl -s -o /dev/null -w '%{{http_code}} ' {SOCKS_ARG} https://www.google.com/robots.txt --max-time 15 & done; wait; echo")
    codes = deploy.run(c, cmd, tmo=40).split()
    ok = sum(1 for x in codes if x=="200")
    print(f"  [{tag}] {ok}/{n} 成功  codes={' '.join(codes)}")
    return ok


def coldtest(n=20):
    """只重启 entry(systemd 管理) -> 连压三轮, 验证'新鲜连接扛得住 / 崩溃是累积'。"""
    cg = deploy.connect("entry")
    cm = deploy.connect("middle")
    print("=== 重启 entry(保留 middle/exit) ===")
    deploy.run(cg, f"systemctl restart {deploy._service_name('entry')}; sleep 2; systemctl is-active {deploy._service_name('entry')}")
    deploy.run(cg, f": > {LOG('entry')}")  # 清 entry 日志便于差量
    print("entry:", deploy.run(cg, "pgrep -x nb_node>/dev/null&&echo ALIVE||echo DEAD").strip())
    mb = _counts(cm, "middle")
    for rnd in range(1, 4):
        _one_round(cg, n, f"round{rnd}")
        time.sleep(1)
    ma = _counts(cm, "middle")
    print(f"middle NEXTHOP Δ(三轮共{3*n}发) = {ma.get('NEXTHOP',0)-mb.get('NEXTHOP',0)}")
    ec = _counts(cg, "entry")
    print(f"entry CONNECT={ec.get('CONNECT',0)} PSFREE={ec.get('PSFREE',0)} "
          f"(泄漏={ec.get('CONNECT',0)-ec.get('PSFREE',0)})")
    print(deploy.run(cg, f"echo SS_{SOCKS_PORT}=$(ss -tan 2>/dev/null|grep -c :{SOCKS_PORT})"))
    cg.close(); cm.close()


def throughput(size_mb=50):
    """隧道吞吐基线: exit 本地起 http.server 大文件, entry 经三跳 SOCKS 下载。
    测 单流 / 5并发 / 20并发 总吞吐(Mbps), 定位并发是否退化(排队)。"""
    cg = deploy.connect("entry"); ck = deploy.connect("exit")
    print(f"=== 准备 exit 本地大文件 {size_mb}MB + http.server:9000 ===")
    deploy.run(ck, f"mkdir -p {WORK}/www; "
                   f"[ -f {WORK}/www/big.bin ] || head -c {size_mb*1000000} /dev/urandom >{WORK}/www/big.bin; "
                   f"pgrep -f 'http.server 9000'>/dev/null || (cd {WORK}/www && setsid nohup python3 -m http.server 9000 </dev/null >/tmp/http.log 2>&1 &); "
                   f"sleep 1; ls -l {WORK}/www/big.bin|awk '{{print $5}}'")
    url = "http://127.0.0.1:9000/big.bin"  # exit 本地 -> 纯测三跳 QUIC 隧道吞吐
    def speed_cmd(n):
        # n 路并发下载, 各自打印 speed_download(bytes/s), 求和=总吞吐
        return (f"for i in $(seq 1 {n}); do curl -o /dev/null -s -w '%{{speed_download}}\\n' "
                f"{SOCKS_ARG} {url} --max-time 90 & done; wait")
    for n in (1, 5, 20):
        out = deploy.run(cg, speed_cmd(n), tmo=120)
        sp = [float(x) for x in out.split() if x.replace('.','',1).replace('e','',1).replace('+','',1).isdigit()]
        total_mbps = sum(sp)*8/1e6
        each = [f"{s*8/1e6:.1f}" for s in sp]
        print(f"[{n:2d}并发] 总={total_mbps:7.1f} Mbps  单流={' '.join(each)} Mbps  ({len(sp)}/{n}完成)")
    cg.close(); ck.close()


if __name__ == "__main__":
    act = sys.argv[1] if len(sys.argv)>1 else "probe"
    if act == "probe": probe()
    elif act == "stress": stress(int(sys.argv[2]) if len(sys.argv)>2 else 20)
    elif act == "coldtest": coldtest(int(sys.argv[2]) if len(sys.argv)>2 else 20)
    elif act == "throughput": throughput(int(sys.argv[2]) if len(sys.argv)>2 else 50)
    else: print(__doc__)
