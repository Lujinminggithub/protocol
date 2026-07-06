#!/usr/bin/env python3
"""Newbility(NB) 三跳部署工具 —— entry(gz) / middle(hk) / exit(kz)。

角色与机器由 lab-hosts.json 描述; middle 经 entry 跳板连接(direct-tcpip)。
NB 节点是一份二进制 nb_node, 靠 -r 选角色。egress/middle 可能无编译器,
故支持"在某台有 picoquic 的机器编译后分发"。

动作:
  recon        只读侦察: 架构/gcc/picoquic/certs/现有 nb 进程, 不改动。
  build        上传 src 到各机, 本地(有 picoquic 者)编译 nb_node。
  deploy-tri   分发二进制 + 起 exit->middle->entry + 冒烟(多 stream + md5)。
  stop         停三跳所有 nb_node 进程。
  logs         拉三跳最近日志(便于跨跳追踪 route+stream_id)。

用法: python deploy.py <action> [--target host:port]
"""
from __future__ import annotations
import argparse, io, json, pathlib, shutil, tarfile, time, sys
import paramiko

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parents[1]         # E:/project/NB
SRC = ROOT / "src"
LAB = json.loads((ROOT / "tools" / "lab-hosts.json").read_text(encoding="utf-8"))
WORK = LAB["paths"]["work_dir"]
DEPLOY_CERTS = f"{WORK}/certs"        # 三跳统一部署证书路径
BUILD_HOST = "exit"                   # 负责编译的机器(自包含后任意 x86_64 皆可)
BUILD_DIR = ROOT / "build"
PLATFORM = "linux-x86_64"             # 目标平台(三跳均 x86_64)
VENDOR_CERTS = ROOT / "third_party" / "picoquic" / "src" / "certs"  # vendored 测试证书
DEFAULT_SOCKS_PORT = 1080
WHITELIST_LOCAL = ROOT / "tools" / "whitelist.local.conf"

# 白名单默认模板(仅首次部署推送到 exit; 之后以服务器 /root/nb/whitelist.conf 为准, 远程编辑即热重载)。
# 语法: domain <后缀> | ip <a.b.c.d/len> | port <端口>; 命中(host AND port)才走隧道, 未命中拒绝。
WL_DEFAULT = """# NB 白名单(访问控制): 命中(域名后缀/IP CIDR/端口)才走三跳隧道, 未命中 exit 拒绝。
# 远程编辑本文件后 ~5s 自动热重载生效, 无需重启。
domain tiktok.com
domain tiktokv.com
domain tiktokcdn.com
domain tiktokcdn-us.com
domain byteoversea.com
domain ibyteimg.com
domain tiktok-row.net
domain ttwstatic.com
domain musical.ly
domain ipinfo.io
port 443
port 80
"""


def _role_host(role):  # role -> dict
    return LAB[role]


def connect(role) -> paramiko.SSHClient:
    """建立 SSH(含 entry 跳板)。失败重试 3 次(全局规范: invoke 不中断)。"""
    h = _role_host(role)
    last = None
    for attempt in range(3):
        try:
            sock = None
            jump = None
            if h.get("jump_via"):
                jrole = h["jump_via"]
                jh = _role_host(jrole)
                jump = paramiko.SSHClient(); jump.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                jump.connect(hostname=jh["host"], port=jh["port"], username=jh["user"],
                             password=jh["password"], timeout=25, banner_timeout=25, auth_timeout=25,
                             allow_agent=False, look_for_keys=False)
                tgt = h.get("jump_target_host") or h["host"]
                sock = jump.get_transport().open_channel("direct-tcpip", (tgt, h["port"]), ("127.0.0.1", 0))
            c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            c.connect(hostname=h["host"], port=h["port"], username=h["user"], password=h["password"],
                      timeout=25, banner_timeout=25, auth_timeout=25, allow_agent=False,
                      look_for_keys=False, sock=sock)
            setattr(c, "_jump", jump)
            return c
        except Exception as e:  # noqa
            last = e; time.sleep(2)
    raise RuntimeError(f"connect {role}({h['name']}) failed after retries: {last}")


def run(c, cmd, tmo=120):
    _i, o, e = c.exec_command(cmd, timeout=tmo)
    return o.read().decode("utf-8", "replace") + e.read().decode("utf-8", "replace")


def launch(c, cmd, warmup=2.0):
    """启动后台服务, 不等 channel EOF(后台进程持有 stdout fd 会致 read 永久阻塞)。"""
    ch = c.get_transport().open_session()
    ch.exec_command(cmd)
    time.sleep(warmup)
    try:
        ch.close()
    except Exception:
        pass


def put_tar(c, files: dict, remote_dir: str):
    """把 {arcname: local_path} 打 tar 传到 remote_dir 解开。"""
    run(c, f"mkdir -p {remote_dir}")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as t:
        for arc, lp in files.items():
            t.add(str(lp), arcname=arc)
    buf.seek(0)
    sf = c.open_sftp(); sf.putfo(buf, f"{remote_dir}/_src.tar"); sf.close()
    run(c, f"cd {remote_dir} && tar xf _src.tar && rm -f _src.tar")


def fetch_bytes(c, path):
    sf = c.open_sftp(); bio = io.BytesIO(); sf.getfo(path, bio); sf.close(); return bio.getvalue()


def push_bytes(c, data, path, mode=0o644):
    run(c, f"mkdir -p $(dirname {path})")
    tmp = f"{path}.tmp"
    sf = c.open_sftp()
    sf.putfo(io.BytesIO(data), tmp)
    sf.chmod(tmp, mode)
    sf.close()
    run(c, f"mv -f {tmp} {path}")


def _service_name(role):
    return f"nb-{role}"


def _service_exists(c, role):
    unit = _service_name(role)
    return run(c, f"systemctl cat {unit} >/dev/null 2>&1 && echo yes || echo no").strip() == "yes"


def _systemd_restart(c, role, warmup=2.0):
    unit = _service_name(role)
    mainpid = run(c, f"systemctl restart {unit}; sleep {warmup}; systemctl is-active {unit}; "
                     f"systemctl show -p MainPID --value {unit}").strip().splitlines()
    return " ".join(x.strip() for x in mainpid if x.strip())


def _systemd_stop(c, role):
    unit = _service_name(role)
    return run(c, f"systemctl stop {unit} 2>/dev/null || true; "
                  "pkill -9 -x nb_node 2>/dev/null || true; echo stopped")


def _require_local_build():
    if not (BUILD_DIR / "nb_node").exists():
        sys.exit("本地缺少 build/nb_node，请先运行: python tools/deploy.py build")


def _whitelist_remote():
    return f"{WORK}/whitelist.conf"


def _ensure_remote_whitelist(c):
    wl_remote = _whitelist_remote()
    exists = run(c, f"test -f {wl_remote} && echo EXISTS || echo NONE").strip()
    if "EXISTS" not in exists:
        push_bytes(c, WL_DEFAULT.encode("utf-8"), wl_remote)
        print(f"whitelist: 内置默认模板已推送 -> exit:{wl_remote}(后续远程编辑此文件即可)")
    else:
        print(f"whitelist: exit 已有 {wl_remote}(保留远程配置, 不覆盖)")
    return wl_remote


def _push_whitelist(c, local_path: pathlib.Path):
    if not local_path.exists():
        sys.exit(f"白名单文件不存在: {local_path}")
    wl_remote = _whitelist_remote()
    push_bytes(c, local_path.read_bytes(), wl_remote)
    print(f"whitelist: 已下发 {local_path} -> exit:{wl_remote}")
    return wl_remote


def _middle_override_path():
    return "/etc/systemd/system/nb-middle.service.d/override.conf"


def _middle_fec_enabled(c):
    if _service_exists(c, "middle"):
        out = run(c, f"systemctl show -p Environment --value {_service_name('middle')} 2>/dev/null")
        return "NB_FEC=on" in out or "NB_FEC=1" in out
    out = run(c,
        "pid=$(pgrep -xo nb_node 2>/dev/null || true); "
        "if [ -n \"$pid\" ] && [ -r /proc/$pid/environ ]; then "
        "tr '\\0' '\\n' </proc/$pid/environ | grep '^NB_FEC=' || true; fi")
    return "NB_FEC=on" in out or "NB_FEC=1" in out


def _legacy_start_cmd(role, socks_port=DEFAULT_SOCKS_PORT, wl_remote=None, fec_enabled=False):
    hk = _role_host("middle")
    kz = _role_host("exit")
    cert = f"{DEPLOY_CERTS}/cert.pem"
    key = f"{DEPLOY_CERTS}/key.pem"
    if role == "entry":
        mid = f"H:{kz['host']}:4443"
        return (f"cd {WORK} && setsid nohup ./nb_node -r entry -l {socks_port} -n {hk['host']} -N 4443 "
                f"-S -M '{mid}' </dev/null >/tmp/nb_entry.log 2>&1 &")
    if role == "middle":
        env = "env NB_FEC=on " if fec_enabled else ""
        return (f"cd {WORK} && setsid nohup {env}./nb_node -r middle -p 4443 -c {cert} -k {key} "
                "</dev/null >/tmp/nb_middle.log 2>&1 &")
    if role == "exit":
        if not wl_remote:
            raise ValueError("legacy exit 启动需要 whitelist 路径")
        return (f"cd {WORK} && setsid nohup ./nb_node -r exit -p 4443 -c {cert} -k {key} -W {wl_remote} "
                "</dev/null >/tmp/nb_exit.log 2>&1 &")
    raise ValueError(f"unknown role: {role}")


def _restart_role(c, role, legacy_cmd, warmup=2.0):
    if _service_exists(c, role):
        return "systemd " + _systemd_restart(c, role, warmup=warmup)
    run(c, "pkill -9 -x nb_node 2>/dev/null || true")
    launch(c, legacy_cmd, warmup=warmup)
    proc = run(c, "pgrep -ax nb_node 2>/dev/null | tail -1").strip()
    return "legacy " + (proc or "started")


def _set_middle_fec(enabled: bool):
    c = connect("middle")
    if _service_exists(c, "middle"):
        override = _middle_override_path()
        if enabled:
            push_bytes(c, b"[Service]\nEnvironment=NB_FEC=on\n", override)
        else:
            run(c, f"rm -f {override}")
        run(c, "systemctl daemon-reload")
        state = "systemd " + _systemd_restart(c, "middle")
    else:
        state = _restart_role(c, "middle", _legacy_start_cmd("middle", fec_enabled=enabled))
    tail = run(c, f"tail -8 {WORK}/logs/nb-middle.log 2>/dev/null")
    c.close()
    action = "ENABLED" if enabled else "DISABLED"
    print(f"middle FEC {action}: {state}")
    if tail.strip():
        print(tail)


def act_fec_status():
    c = connect("middle")
    runtime = "systemd" if _service_exists(c, "middle") else "legacy"
    override = _middle_override_path()
    out = run(c,
        f"echo RUNTIME={runtime}; "
        f"echo ACTIVE=$(systemctl is-active {_service_name('middle')} 2>/dev/null || echo unknown); "
        f"echo ENV=$(systemctl show -p Environment --value {_service_name('middle')} 2>/dev/null); "
        f"echo DROPIN=$(systemctl show -p DropInPaths --value {_service_name('middle')} 2>/dev/null); "
        f"echo PROC=$(pgrep -ax nb_node 2>/dev/null | tail -1); "
        f"echo '--- override ---'; cat {override} 2>/dev/null || echo '(no override)'; "
        f"echo '--- log tail ---'; tail -8 {WORK}/logs/nb-middle.log 2>/dev/null")
    print(out)
    c.close()


def _smoke_socks(socks_port=DEFAULT_SOCKS_PORT):
    cg = connect("entry")
    kz_ip = _role_host("exit")["host"]
    smoke = run(cg,
        "pgrep -x nb_node>/dev/null&&echo NB_ENTRY_UP||echo DOWN\n"
        f"echo '--- 经 SOCKS5 三跳出口 IP(应=kz {kz_ip}) ---'\n"
        f"curl -s --socks5-hostname 127.0.0.1:{socks_port} http://ipinfo.io/ip --max-time 20; echo\n"
        "echo '--- gz 直连出口 IP(对照) ---'; curl -s http://ipinfo.io/ip --max-time 10; echo\n"
        f"echo '--- entry log ---'; tail -6 {WORK}/logs/nb-entry.log 2>/dev/null", tmo=70)
    cg.close()
    print("=== SOCKS5 冒烟(gz entry) ===\n" + smoke)


BUILD_CMD = (
    f"cd {WORK} && rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/tmp/nbcmake.log 2>&1; "
    f"cmake --build build -j$(nproc) >>/tmp/nbcmake.log 2>&1; echo cmake_rc=$?; tail -3 /tmp/nbcmake.log"
)

# CMake 构建集: 自有源码 + CMakeLists + build_libs.sh + 目标平台预编译 .a(自包含, 零依赖 /root/poc)
BUILD_FILES = {
    "src/nb_node.c": SRC / "nb_node.c",
    "src/log/log4c.c": SRC / "log" / "log4c.c",
    "src/log/log4c.h": SRC / "log" / "log4c.h",
    "CMakeLists.txt": ROOT / "CMakeLists.txt",
    "third_party/picoquic/build_libs.sh": ROOT / "third_party" / "picoquic" / "build_libs.sh",
}
for _a in sorted((ROOT / "third_party" / "picoquic" / "prebuilt" / PLATFORM).glob("*.a")):
    BUILD_FILES[f"third_party/picoquic/prebuilt/{PLATFORM}/{_a.name}"] = _a
# prebuilt 路径编译 nb_node.c 需要 picoquic 头文件(CMake 的 3 个 include 目录, 仅头文件不传完整 src)
for _sub in ("src/picoquic", "src/loglib", "src/picotls/include"):
    _base = ROOT / "third_party" / "picoquic" / _sub
    for _hf in _base.rglob("*.h"):
        BUILD_FILES[_hf.relative_to(ROOT).as_posix()] = _hf


def act_recon(roles):
    for r in roles:
        c = connect(r); h = _role_host(r)
        out = run(c, "echo arch=$(uname -m); echo gcc=$(gcc -dumpversion 2>/dev/null||echo NONE); "
                     "echo cmake=$(cmake --version 2>/dev/null|head -1|awk '{print $3}'||echo NONE); "
                     "echo openssl_dev=$(ls /usr/include/openssl/ssl.h 2>/dev/null||echo NONE); "
                     f"echo nb_bin=$(ls {WORK}/build/nb_node 2>/dev/null||echo NONE); "
                     "echo proc=$(pgrep -x nb_node|tr '\\n' ',' || echo none)")
        print(f"### {r}({h['name']}) {h['host']}:{h['port']}\n{out}")
        c.close()


def act_build(roles):
    """CMake + vendored 构建(自包含, 零依赖 /root/poc); 产物 + vendored 证书下载到 build/。"""
    c = connect(BUILD_HOST); h = _role_host(BUILD_HOST)
    print(f"### build on {BUILD_HOST}({h['name']}) via CMake + vendored picoquic ...")
    run(c, f"rm -rf {WORK}/src {WORK}/third_party {WORK}/CMakeLists.txt {WORK}/build; mkdir -p {WORK}")
    put_tar(c, BUILD_FILES, WORK)
    out = run(c, BUILD_CMD, tmo=300)
    print(out.strip()[-600:] if out.strip() else "(no output)")
    ok = run(c, f"ls -l {WORK}/build/nb_node 2>/dev/null && echo BUILD_OK || echo BUILD_FAIL")
    print(ok)
    if "BUILD_OK" not in ok:
        c.close(); sys.exit("编译失败, 中止")
    BUILD_DIR.mkdir(exist_ok=True)
    (BUILD_DIR / "nb_node").write_bytes(fetch_bytes(c, f"{WORK}/build/nb_node"))
    shutil.copy(VENDOR_CERTS / "cert.pem", BUILD_DIR / "cert.pem")
    shutil.copy(VENDOR_CERTS / "key.pem", BUILD_DIR / "key.pem")
    print(f"产物 -> {BUILD_DIR}: nb_node + vendored certs")
    c.close()


def _distribute(role, need_cert):
    """把本地 build/ 的二进制(+证书)推到某角色机。先停旧进程释放文件(防 ETXTBSY)。"""
    c = connect(role); h = _role_host(role)
    run(c, f"pkill -9 -x nb_node 2>/dev/null; sleep 0.3; rm -f {WORK}/nb_node; mkdir -p {WORK}/logs {WORK}/www; echo ok")
    push_bytes(c, (BUILD_DIR / "nb_node").read_bytes(), f"{WORK}/nb_node", mode=0o755)
    if need_cert:
        push_bytes(c, (BUILD_DIR / "cert.pem").read_bytes(), f"{DEPLOY_CERTS}/cert.pem")
        push_bytes(c, (BUILD_DIR / "key.pem").read_bytes(), f"{DEPLOY_CERTS}/key.pem")
    print(f"分发 -> {role}({h['name']}) done")
    return c


def act_stop(roles):
    for r in roles:
        c = connect(r); _systemd_stop(c, r); c.close()
    print("三跳 nb_node 已停")


def act_logs(roles):
    for r in roles:
        c = connect(r); h = _role_host(r)
        out = run(c, f"tail -25 {WORK}/logs/nb-*.log 2>/dev/null || echo '(no log)'")
        print(f"### {r}({h['name']}) log\n{out}")
        c.close()


def act_deploy_tri():
    """分发二进制/证书 -> 起 exit(kz)->middle(hk)->entry(gz) -> 从 gz 冒烟(多 stream + md5)。"""
    hk = _role_host("middle"); kz = _role_host("exit")
    hk_ip = hk["host"]; kz_ip = kz["host"]
    cert = f"{DEPLOY_CERTS}/cert.pem"; key = f"{DEPLOY_CERTS}/key.pem"
    # 1) exit(kz): 本机已有 build 产物; 起 http.server(目标) + nb exit
    ck = connect("exit")
    run(ck, f"mkdir -p {WORK}/www {WORK}/logs; "
            f"echo HELLO_NB_TUNNEL_OK>{WORK}/www/test.txt; head -c 300000 /dev/urandom|base64>{WORK}/www/big.txt; "
            f"pkill -9 -x nb_node; pkill -9 -f 'python3 -m http.server'; echo prepared")
    launch(ck, f"cd {WORK}/www && setsid nohup python3 -m http.server 9000 </dev/null >/tmp/http.log 2>&1 & "
               f"cd {WORK} && setsid nohup ./nb_node -r exit -p 4443 -c {cert} -k {key} </dev/null >/tmp/nb_exit.log 2>&1 &")
    print("exit(kz):", run(ck, "pgrep -x nb_node>/dev/null&&echo NB_EXIT_UP||echo DOWN; tail -3 /tmp/nb_exit.log"))
    # 2) middle(hk): 分发 + 起
    cm = _distribute("middle", need_cert=True)
    run(cm, "pkill -9 -x nb_node; echo ok")
    launch(cm, f"cd {WORK} && setsid nohup ./nb_node -r middle -p 4443 -c {cert} -k {key} </dev/null >/tmp/nb_middle.log 2>&1 &")
    print("middle(hk):", run(cm, "pgrep -x nb_node>/dev/null&&echo NB_MIDDLE_UP||echo DOWN; tail -3 /tmp/nb_middle.log"))
    # 3) entry(gz): 分发 + 起; route = 经 middle(kz地址) 到 exit, exit 连本地 http
    cg = _distribute("entry", need_cert=False)
    route = f"H:{kz_ip}:4443,T:127.0.0.1:9000"
    run(cg, "pkill -9 -x nb_node; echo ok")
    launch(cg, f"cd {WORK} && setsid nohup ./nb_node -r entry -l 8080 -n {hk_ip} -N 4443 -R '{route}' </dev/null >/tmp/nb_entry.log 2>&1 &")
    time.sleep(2)
    smoke = run(cg,
        "pgrep -x nb_node>/dev/null&&echo NB_ENTRY_UP||echo DOWN\n"
        "for i in 1 2 3 4 5; do curl -s -o /dev/null -w \"try$i first_byte=%{time_starttransfer}s http=%{http_code}\\n\" "
        "http://127.0.0.1:8080/test.txt --max-time 12; done\n"
        "echo '--- md5 300KB ---'; curl -s http://127.0.0.1:8080/big.txt -o /tmp/gb --max-time 20; "
        "echo gz_md5=$(md5sum /tmp/gb 2>/dev/null|awk '{print $1}')\n"
        f"echo '--- entry log ---'; tail -6 {WORK}/logs/nb-entry.log 2>/dev/null", tmo=90)
    print("=== 三跳冒烟(gz entry) ===\n" + smoke)
    print("=== kz big.txt md5 ===", run(ck, f"md5sum {WORK}/www/big.txt|awk '{{print $1}}'"))
    print("=== middle log ===\n" + run(cm, f"tail -8 {WORK}/logs/nb-middle.log 2>/dev/null"))
    print("=== exit log ===\n" + run(ck, f"tail -8 {WORK}/logs/nb-exit.log 2>/dev/null"))
    for c in (cg, cm, ck):
        c.close()


def act_deploy_socks(socks_port=DEFAULT_SOCKS_PORT):
    """分发本地 build/nb_node -> systemd 重启 exit/middle/entry -> SOCKS5 冒烟。"""
    _require_local_build()
    gz = _role_host("entry")
    bindata = (BUILD_DIR / "nb_node").read_bytes()
    cert = (BUILD_DIR / "cert.pem").read_bytes()
    key = (BUILD_DIR / "key.pem").read_bytes()

    ck = connect("exit")
    run(ck, f"mkdir -p {WORK}/logs")
    push_bytes(ck, bindata, f"{WORK}/nb_node", mode=0o755)
    push_bytes(ck, cert, f"{DEPLOY_CERTS}/cert.pem")
    push_bytes(ck, key, f"{DEPLOY_CERTS}/key.pem")
    wl_remote = _ensure_remote_whitelist(ck)
    print("exit(kz):", _restart_role(ck, "exit", _legacy_start_cmd("exit", wl_remote=wl_remote)))
    print(run(ck, f"tail -4 {WORK}/logs/nb-exit.log 2>/dev/null"))
    ck.close()

    cm = connect("middle")
    push_bytes(cm, bindata, f"{WORK}/nb_node", mode=0o755)
    push_bytes(cm, cert, f"{DEPLOY_CERTS}/cert.pem")
    push_bytes(cm, key, f"{DEPLOY_CERTS}/key.pem")
    print("middle(hk):", _restart_role(cm, "middle",
        _legacy_start_cmd("middle", fec_enabled=_middle_fec_enabled(cm))))
    print(run(cm, f"tail -4 {WORK}/logs/nb-middle.log 2>/dev/null"))
    cm.close()

    cg = connect("entry")
    push_bytes(cg, bindata, f"{WORK}/nb_node", mode=0o755)
    print("entry(gz):", _restart_role(cg, "entry", _legacy_start_cmd("entry", socks_port=socks_port)))
    print(run(cg, f"tail -4 {WORK}/logs/nb-entry.log 2>/dev/null"))
    cg.close()

    _smoke_socks(socks_port)
    print(f"\n>>> 手机配置: Shadowrocket 新建 SOCKS5 代理 -> {gz['host']}:{socks_port} (无认证)")


def act_wl_show():
    c = connect("exit")
    print(run(c, f"cat {_whitelist_remote()}"))
    c.close()


def act_wl_push(local_path: pathlib.Path):
    c = connect("exit")
    _push_whitelist(c, local_path)
    print(run(c, f"tail -6 {WORK}/logs/nb-exit.log 2>/dev/null"))
    c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=[
        "recon", "build", "deploy-tri", "deploy-socks", "stop", "logs",
        "wl-show", "wl-push", "fec-status", "fec-on", "fec-off"
    ])
    ap.add_argument("--roles", default="entry,middle,exit")
    ap.add_argument("--socks-port", type=int, default=DEFAULT_SOCKS_PORT)
    ap.add_argument("--whitelist", default=str(WHITELIST_LOCAL))
    a = ap.parse_args()
    roles = [r.strip() for r in a.roles.split(",") if r.strip()]
    if a.action == "recon": act_recon(roles)
    elif a.action == "build": act_build(roles)
    elif a.action == "deploy-tri": act_deploy_tri()
    elif a.action == "deploy-socks": act_deploy_socks(a.socks_port)
    elif a.action == "stop": act_stop(roles)
    elif a.action == "logs": act_logs(roles)
    elif a.action == "wl-show": act_wl_show()
    elif a.action == "wl-push": act_wl_push(pathlib.Path(a.whitelist))
    elif a.action == "fec-status": act_fec_status()
    elif a.action == "fec-on": _set_middle_fec(True)
    elif a.action == "fec-off": _set_middle_fec(False)


if __name__ == "__main__":
    main()
