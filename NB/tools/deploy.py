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
import argparse, io, ipaddress, json, pathlib, tarfile, time, sys, os, shlex
import paramiko

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parents[1]         # E:/project/NB
SRC = ROOT / "src"
LAB_FILE = pathlib.Path(os.environ.get("NB_HOSTS_FILE", str(ROOT / "tools" / "lab-hosts.json")))
LAB = json.loads(LAB_FILE.read_text(encoding="utf-8"))
WORK = LAB["paths"]["work_dir"]
DEPLOY_CERTS = f"{WORK}/certs"        # 三跳统一部署证书路径
BUILD_HOST = LAB.get("build_host", "exit")                   # 负责编译的机器(自包含后任意 x86_64 皆可)
BUILD_DIR = ROOT / "build"
PLATFORM = "linux-x86_64"             # 目标平台(三跳均 x86_64)
SECURITY_DIR = pathlib.Path(os.environ.get("NB_SECURITY_DIR", str(BUILD_DIR / "security")))
DEFAULT_SOCKS_PORT = 1080
WHITELIST_LOCAL = ROOT / "tools" / "whitelist.local.conf"

# 白名单默认模板(仅首次部署推送到 exit; 之后以服务器 /root/nb/whitelist.conf 为准, 远程编辑即热重载)。
# 语法: domain <后缀> | ip <a.b.c.d/len> | port <端口>; 命中(host AND port)才走隧道, 未命中拒绝。
WL_DEFAULT = """# NB 白名单(访问控制): 命中(域名后缀/IP CIDR/端口)才走三跳隧道, 未命中 exit 拒绝。
# 远程编辑本文件后 ~5s 自动热重载生效, 无需重启。
domain tiktok.com
domain tiktokv.com
domain tiktokv.us
domain tiktokcdn.com
domain tiktokcdn.us
domain tiktokcdn-us.com
domain byteoversea.com
domain ibyteimg.com
domain tiktok-row.net
domain ttwstatic.com
domain musical.ly
domain ipinfo.io
domain ip.sb
domain www.google.com
port 443
port 80
port 50008
port 50009
"""


def _role_host(role):  # role -> dict
    return LAB[role]


def _host_password(host: dict) -> str:
    env_name = host.get("password_env")
    if not env_name or not os.environ.get(env_name):
        raise RuntimeError(f"{host['name']} 缺少 SSH 密码环境变量: {env_name or 'password_env'}")
    return os.environ[env_name]


def _configure_host_keys(client: paramiko.SSHClient) -> None:
    known_hosts = os.environ.get("NB_KNOWN_HOSTS")
    if known_hosts:
        client.load_host_keys(known_hosts)
        client.set_missing_host_key_policy(paramiko.RejectPolicy())
    elif os.environ.get("NB_SSH_INSECURE") == "1":
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    else:
        raise RuntimeError("缺少 NB_KNOWN_HOSTS；首次引导可显式设置 NB_SSH_INSECURE=1")


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
                jump = paramiko.SSHClient(); _configure_host_keys(jump)
                jump.connect(hostname=jh["host"], port=jh["port"], username=jh["user"],
                             password=_host_password(jh), timeout=25, banner_timeout=25, auth_timeout=25,
                             allow_agent=False, look_for_keys=False)
                tgt = h.get("jump_target_host") or h["host"]
                sock = jump.get_transport().open_channel("direct-tcpip", (tgt, h["port"]), ("127.0.0.1", 0))
            c = paramiko.SSHClient(); _configure_host_keys(c)
            c.connect(hostname=h["host"], port=h["port"], username=h["user"], password=_host_password(h),
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
    push_bytes(c, buf.getvalue(), f"{remote_dir}/_src.tar")
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


def _require_security_material():
    required = [SECURITY_DIR / "ca.pem", SECURITY_DIR / "socks.users"]
    required += [SECURITY_DIR / f"{role}.{ext}" for role in ("entry", "middle", "exit") for ext in ("pem", "key")]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        sys.exit("缺少安全材料，请先运行 tools/security_setup.py:\n" + "\n".join(missing))


def _remote_security(role):
    return {
        "ca": f"{DEPLOY_CERTS}/ca.pem",
        "cert": f"{DEPLOY_CERTS}/{role}.pem",
        "key": f"{DEPLOY_CERTS}/{role}.key",
        "users": f"{WORK}/socks.users",
    }


def _push_security(c, role):
    _require_security_material()
    paths = _remote_security(role)
    push_bytes(c, (SECURITY_DIR / "ca.pem").read_bytes(), paths["ca"], mode=0o644)
    push_bytes(c, (SECURITY_DIR / f"{role}.pem").read_bytes(), paths["cert"], mode=0o644)
    push_bytes(c, (SECURITY_DIR / f"{role}.key").read_bytes(), paths["key"], mode=0o600)
    if role == "entry":
        push_bytes(c, (SECURITY_DIR / "socks.users").read_bytes(), paths["users"], mode=0o600)


def _whitelist_remote():
    return f"{WORK}/whitelist.conf"


def _tiktok_rules_remote():
    return f"{WORK}/tiktok_flow_rules.conf"


def _exit_routes_remote():
    return f"{WORK}/exit_routes.conf"


def _push_exit_routes(c):
    exits=LAB.get("exits") or [{"name":_role_host("exit")["name"],"host":_role_host("exit")["host"],"port":4443,"weight":1}]
    lines=["# route <name> <H:host:port> <weight>"]
    for item in exits:
        name=str(item["name"]);host=str(item["host"]);port=int(item.get("port",4443));weight=int(item.get("weight",1))
        if not name.replace("-","").replace("_","").isalnum() or not 1<=port<=65535 or not 1<=weight<=1000:
            raise ValueError(f"非法出口路由配置: {item}")
        lines.append(f"route {name} H:{host}:{port} {weight}")
    push_bytes(c,("\n".join(lines)+"\n").encode("ascii"),_exit_routes_remote(),mode=0o644)
    return _exit_routes_remote()


def _ensure_remote_whitelist(c, role="exit"):
    wl_remote = _whitelist_remote()
    exists = run(c, f"test -f {wl_remote} && echo EXISTS || echo NONE").strip()
    if "EXISTS" not in exists:
        push_bytes(c, WL_DEFAULT.encode("utf-8"), wl_remote)
        print(f"whitelist: 内置默认模板已推送 -> {role}:{wl_remote}(后续远程编辑此文件即可)")
    else:
        print(f"whitelist: {role} 已有 {wl_remote}(保留远程配置, 不覆盖)")
    return wl_remote


def _push_whitelist(c, local_path: pathlib.Path):
    if not local_path.exists():
        sys.exit(f"白名单文件不存在: {local_path}")
    wl_remote = _whitelist_remote()
    push_bytes(c, local_path.read_bytes(), wl_remote)
    print(f"whitelist: 已下发 {local_path} -> exit:{wl_remote}")
    return wl_remote


def _push_tiktok_rules(c):
    local_rules = ROOT / "tools" / "tiktok_flow_rules.conf"
    if local_rules.exists():
        push_bytes(c, local_rules.read_bytes(), _tiktok_rules_remote())


def _fec_override_path(role):
    return f"/etc/systemd/system/nb-{role}.service.d/override.conf"


def _role_fec_enabled(c, role):
    if _service_exists(c, role):
        out = run(c, f"systemctl show -p Environment --value {_service_name(role)} 2>/dev/null")
        return "NB_FEC_V15_ACTIVE=on" in out or "NB_FEC_V15_ACTIVE=1" in out
    out = run(c,
        "pid=$(pgrep -xo nb_node 2>/dev/null || true); "
        "if [ -n \"$pid\" ] && [ -r /proc/$pid/environ ]; then "
        "tr '\\0' '\\n' </proc/$pid/environ | grep '^NB_FEC_V15_ACTIVE=' || true; fi")
    return "NB_FEC_V15_ACTIVE=on" in out or "NB_FEC_V15_ACTIVE=1" in out


def _node_command(role, socks_port=DEFAULT_SOCKS_PORT, wl_remote=None):
    hk = _role_host("middle")
    kz = _role_host("exit")
    sec = _remote_security(role)
    base = f"{WORK}/nb_node -r {role} -c {sec['cert']} -k {sec['key']} -a {sec['ca']}"
    if role == "entry":
        mid = f"H:{kz['host']}:4443"
        middle_data_host = hk.get("private_ip") or hk.get("jump_target_host") or hk["host"]
        if not wl_remote:
            raise ValueError("entry SOCKS 启动需要 whitelist 路径")
        return f"{base} -l {socks_port} -n {middle_data_host} -N 4443 -S -U {sec['users']} -W {wl_remote} -M {shlex.quote(mid)} -E {_exit_routes_remote()}"
    if role == "middle":
        return f"{base} -p 4443"
    if role == "exit":
        if not wl_remote:
            raise ValueError("exit 启动需要 whitelist 路径")
        outip=_role_host("exit").get("outip")
        source=f" -o {outip}" if outip else ""
        return f"{base} -p 4443 -W {wl_remote}{source}"
    raise ValueError(f"unknown role: {role}")


def _install_and_restart_role(c, role, command, warmup=2.0):
    workers=int(LAB.get("workers",{}).get(role,1))
    if not 1<=workers<=32:
        raise ValueError(f"workers.{role} 必须为 1..32")
    if role=="middle" and workers>1:
        print("middle worker 强制降为 1：当前共享 UDP :4443 的客户端回包无法跨 picoquic context 分派")
        workers=1
    supervisor=f"{WORK}/nb_supervisor.py"
    push_bytes(c,(ROOT/"tools"/"nb_supervisor.py").read_bytes(),supervisor,mode=0o755)
    exec_start=f"/usr/bin/python3 {supervisor} --workers {workers} -- {command}"
    transport=LAB.get("transport",{}).get(role,{})
    cc=str(transport.get("cc","bbr")).lower()
    if cc not in ("bbr","cubic","dcubic","fastcc","reno"):
        raise ValueError(f"transport.{role}.cc 非法: {cc}")
    bbr_options=str(transport.get("bbr_options","Q0.0001:"))
    if any(ch in bbr_options for ch in "\r\n\0"):
        raise ValueError(f"transport.{role}.bbr_options 非法")
    cwin_max_bytes=int(transport.get("cwin_max_bytes",0))
    if cwin_max_bytes != 0 and not 65536<=cwin_max_bytes<=67108864:
        raise ValueError(f"transport.{role}.cwin_max_bytes 必须为 0 或 65536..67108864")
    cwin_env=(f"Environment=NB_CWIN_MAX_BYTES={cwin_max_bytes}\n" if cwin_max_bytes else "")
    udp_advertise_env=""
    if role=="entry":
        entry_host=_role_host("entry")
        udp_advertise_ip=str(transport.get("udp_advertise_ip") or
            entry_host.get("public_ip") or entry_host["host"]).strip()
        if udp_advertise_ip:
            try:
                ipaddress.IPv4Address(udp_advertise_ip)
            except ipaddress.AddressValueError as exc:
                raise ValueError(
                    f"entry UDP 公网地址非法: {udp_advertise_ip}; 域名登录场景请设置 entry.public_ip") from exc
            udp_advertise_env=f"Environment=NB_SOCKS_UDP_ADVERTISE_IP={udp_advertise_ip}\n"
        udp_port_min=int(transport.get("udp_port_min",0))
        udp_port_max=int(transport.get("udp_port_max",0))
        if bool(udp_port_min)!=bool(udp_port_max) or (udp_port_min and
                (udp_port_min<1024 or udp_port_max>65535 or udp_port_min>udp_port_max or
                 udp_port_max-udp_port_min+1>16384)):
            raise ValueError("transport.entry UDP 端口范围非法")
        if udp_port_min:
            udp_advertise_env+=(f"Environment=NB_SOCKS_UDP_PORT_MIN={udp_port_min}\n"
                                f"Environment=NB_SOCKS_UDP_PORT_MAX={udp_port_max}\n")
    reorder_env=""
    if role in ("entry","middle"):
        reorder_gap=int(transport.get("reorder_gap",3))
        reorder_delay_us=int(transport.get("reorder_delay_us",0))
        if not 3<=reorder_gap<=1024 or not 0<=reorder_delay_us<=2000000:
            raise ValueError(f"transport.{role} reorder 参数越界")
        reorder_env=(f"Environment=NB_REORDER_GAP={reorder_gap}\n"
                     f"Environment=NB_REORDER_DELAY_US={reorder_delay_us}\n")
    unit = f"""[Unit]
Description=Newbility {role} node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
Environment=NB_FEC_V15=on
Environment=NB_UDP_GSO=on
Environment=NB_CC={cc}
Environment=NB_BBR_OPTIONS={bbr_options}
{cwin_env}{udp_advertise_env}{reorder_env}ExecStart={exec_start}
Restart=on-failure
RestartSec=2
KillMode=control-group
LimitNOFILE=1048576
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
"""
    remote = f"/etc/systemd/system/nb-{role}.service"
    push_bytes(c, unit.encode("utf-8"), remote, mode=0o644)
    run(c, f"systemctl stop nb-{role} 2>/dev/null || true; rm -f /run/nb-{role}-*.ctl; "
           f"systemctl daemon-reload && systemctl enable nb-{role} >/dev/null")
    state = _systemd_restart(c, role, warmup=warmup)
    if "active" not in state:
        detail = run(c, f"systemctl status nb-{role} --no-pager -l; journalctl -u nb-{role} -n 30 --no-pager")
        raise RuntimeError(f"nb-{role} 启动失败:\n{detail}")
    return f"systemd workers={workers} " + state


def _set_fec_roles(enabled: bool, roles=("entry", "middle")):
    action = "ENABLED" if enabled else "DISABLED"
    for role in roles:
        c = connect(role)
        if _service_exists(c, role):
            override = _fec_override_path(role)
            if enabled:
                push_bytes(c, b"[Service]\nEnvironment=NB_FEC_V15_ACTIVE=on\n", override)
            else:
                run(c, f"rm -f {override}")
            run(c, "systemctl daemon-reload")
            state = "systemd " + _systemd_restart(c, role)
        else:
            c.close()
            raise RuntimeError(f"nb-{role}.service 不存在，请先执行 deploy-socks")
        tail = run(c, f"tail -8 {WORK}/logs/nb-{role}.log 2>/dev/null")
        c.close()
        print(f"{role} FEC {action}: {state}")
        if tail.strip():
            print(tail)


def act_fec_status():
    for role in ("entry", "middle"):
        c = connect(role)
        runtime = "systemd" if _service_exists(c, role) else "legacy"
        override = _fec_override_path(role)
        out = run(c,
            f"echo ROLE={role}; "
            f"echo RUNTIME={runtime}; "
            f"echo ACTIVE=$(systemctl is-active {_service_name(role)} 2>/dev/null || echo unknown); "
            f"echo ENV=$(systemctl show -p Environment --value {_service_name(role)} 2>/dev/null); "
            f"echo DROPIN=$(systemctl show -p DropInPaths --value {_service_name(role)} 2>/dev/null); "
            f"echo PROC=$(pgrep -ax nb_node 2>/dev/null | tail -1); "
            f"echo '--- override ---'; cat {override} 2>/dev/null || echo '(no override)'; "
            f"echo '--- log tail ---'; tail -8 {WORK}/logs/nb-{role}.log 2>/dev/null")
        print(out)
        c.close()


def _smoke_socks(socks_port=DEFAULT_SOCKS_PORT):
    cg = connect("entry")
    kz_ip = _role_host("exit").get("outip",_role_host("exit")["host"])
    user=os.environ.get("NB_SOCKS_USERNAME");password=os.environ.get("NB_SOCKS_PASSWORD")
    if not user or not password:
        cg.close();raise RuntimeError("SOCKS 冒烟需要 NB_SOCKS_USERNAME 和 NB_SOCKS_PASSWORD")
    auth=shlex.quote(f"{user}:{password}")
    actual=""
    attempts=[]
    for attempt in range(1,6):
        actual=run(cg,f"curl -fsS --proxy-user {auth} --socks5-hostname 127.0.0.1:{socks_port} http://ipinfo.io/ip --max-time 15",tmo=20).strip()
        attempts.append(f"try={attempt} exit={actual or '(empty)'}")
        if actual==kz_ip:break
        time.sleep(3)
    direct=run(cg,"curl -fsS http://ipinfo.io/ip --max-time 10",tmo=15).strip()
    tail=run(cg,f"tail -6 {WORK}/logs/nb-entry.log 2>/dev/null",tmo=15)
    cg.close()
    smoke="\n".join(attempts)+f"\nexpected={kz_ip}\ndirect={direct}\n--- entry log ---\n{tail}"
    print("=== SOCKS5 冒烟(gz entry) ===\n"+smoke)
    if actual!=kz_ip:raise RuntimeError(f"SOCKS 出口验证失败: expected={kz_ip}, actual={actual or '(empty)'}")


REBUILD_PICOQUIC = os.environ.get("NB_REBUILD_PICOQUIC") == "1"
PICOQUIC_REBUILD_CMD = (
    f"rm -rf {WORK}/third_party/picoquic/prebuilt/{PLATFORM} "
    f"{WORK}/third_party/picoquic/src/build-{PLATFORM} "
    f"{WORK}/third_party/picoquic/src/picotls/build-{PLATFORM}; "
    f"bash {WORK}/third_party/picoquic/build_libs.sh {PLATFORM}; "
) if REBUILD_PICOQUIC else ""
BUILD_CMD = (
    PICOQUIC_REBUILD_CMD +
    f"cd {WORK} && rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/tmp/nbcmake.log 2>&1; "
    f"cmake --build build -j$(nproc) >>/tmp/nbcmake.log 2>&1; echo cmake_rc=$?; tail -3 /tmp/nbcmake.log"
)

# CMake 构建集: 自有源码 + CMakeLists + build_libs.sh + 目标平台预编译 .a(自包含, 零依赖 /root/poc)
BUILD_FILES = {
    "src/nb_node.c": SRC / "nb_node.c",
    "src/nb_policy.c": SRC / "nb_policy.c",
    "src/nb_policy.h": SRC / "nb_policy.h",
    "src/nb_fec_rs.c": SRC / "nb_fec_rs.c",
    "src/nb_fec_rs.h": SRC / "nb_fec_rs.h",
    "src/nb_fec.c": SRC / "nb_fec.c",
    "src/nb_fec.h": SRC / "nb_fec.h",
    "src/nb_ring.c": SRC / "nb_ring.c",
    "src/nb_ring.h": SRC / "nb_ring.h",
    "src/nb_auth.c": SRC / "nb_auth.c",
    "src/nb_auth.h": SRC / "nb_auth.h",
    "src/nb_control.c": SRC / "nb_control.c",
    "src/nb_control.h": SRC / "nb_control.h",
    "src/nb_routes.c": SRC / "nb_routes.c",
    "src/nb_routes.h": SRC / "nb_routes.h",
    "src/nb_udp.c": SRC / "nb_udp.c",
    "src/nb_udp.h": SRC / "nb_udp.h",
    "tools/test_fec.c": ROOT / "tools" / "test_fec.c",
    "tools/test_fec_rs.c": ROOT / "tools" / "test_fec_rs.c",
    "tools/test_ring.c": ROOT / "tools" / "test_ring.c",
    "tools/test_auth.c": ROOT / "tools" / "test_auth.c",
    "tools/test_control.c": ROOT / "tools" / "test_control.c",
    "tools/test_routes.c": ROOT / "tools" / "test_routes.c",
    "tools/test_udp.c": ROOT / "tools" / "test_udp.c",
    "src/log/log4c.c": SRC / "log" / "log4c.c",
    "src/log/log4c.h": SRC / "log" / "log4c.h",
    "CMakeLists.txt": ROOT / "CMakeLists.txt",
    "third_party/picoquic/build_libs.sh": ROOT / "third_party" / "picoquic" / "build_libs.sh",
    # 该文件包含 NB 针对长 RTT 随机丢包的 BBRv3 修正；源码构建验证时覆盖 vendored 基线。
    "third_party/picoquic/src/picoquic/bbr.c": ROOT / "third_party" / "picoquic" / "src" / "picoquic" / "bbr.c",
}
for _a in sorted((ROOT / "third_party" / "picoquic" / "prebuilt" / PLATFORM).glob("*.a")):
    BUILD_FILES[f"third_party/picoquic/prebuilt/{PLATFORM}/{_a.name}"] = _a
# prebuilt 路径编译 nb_node.c 需要 picoquic 头文件(CMake 的 3 个 include 目录, 仅头文件不传完整 src)
for _sub in ("src/picoquic", "src/loglib", "src/picotls/include"):
    _base = ROOT / "third_party" / "picoquic" / _sub
    for _hf in _base.rglob("*.h"):
        BUILD_FILES[_hf.relative_to(ROOT).as_posix()] = _hf
if REBUILD_PICOQUIC:
    _source_root = ROOT / "third_party" / "picoquic" / "src"
    for _source_file in _source_root.rglob("*"):
        if not _source_file.is_file():
            continue
        _relative = _source_file.relative_to(_source_root)
        if any(part.startswith("build-") or part == ".git" for part in _relative.parts):
            continue
        BUILD_FILES[_source_file.relative_to(ROOT).as_posix()] = _source_file


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
    """CMake + vendored 构建；证书由 security_setup.py 独立管理。"""
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
    if REBUILD_PICOQUIC:
        local_prebuilt = ROOT / "third_party" / "picoquic" / "prebuilt" / PLATFORM
        local_prebuilt.mkdir(parents=True, exist_ok=True)
        for archive in ("libpicoquic-core.a", "libpicoquic-log.a", "libpicotls-openssl.a",
                        "libpicotls-core.a", "libpicotls-minicrypto.a"):
            (local_prebuilt / archive).write_bytes(fetch_bytes(
                c, f"{WORK}/third_party/picoquic/prebuilt/{PLATFORM}/{archive}"))
    print(f"产物 -> {BUILD_DIR}: nb_node")
    c.close()


def _distribute(role):
    """把二进制和该角色的安全材料推到节点。"""
    c = connect(role); h = _role_host(role)
    run(c, f"pkill -9 -x nb_node 2>/dev/null; sleep 0.3; rm -f {WORK}/nb_node; mkdir -p {WORK}/logs {WORK}/www; echo ok")
    push_bytes(c, (BUILD_DIR / "nb_node").read_bytes(), f"{WORK}/nb_node", mode=0o755)
    _push_security(c, role)
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
    _require_local_build();_require_security_material()
    # 1) exit(kz): 起本地 HTTP 目标和安全 NB exit
    ck = _distribute("exit")
    run(ck, f"mkdir -p {WORK}/www {WORK}/logs; "
            f"echo HELLO_NB_TUNNEL_OK>{WORK}/www/test.txt; head -c 300000 /dev/urandom|base64>{WORK}/www/big.txt; "
            f"pkill -9 -x nb_node; pkill -9 -f 'python3 -m http.server'; echo prepared")
    launch(ck, f"cd {WORK}/www && setsid nohup python3 -m http.server 9000 </dev/null >/tmp/http.log 2>&1 &")
    wl_remote = _ensure_remote_whitelist(ck)
    _install_and_restart_role(ck,"exit",_node_command("exit",wl_remote=wl_remote))
    print("exit(kz):", run(ck, "pgrep -x nb_node>/dev/null&&echo NB_EXIT_UP||echo DOWN; tail -3 /tmp/nb_exit.log"))
    # 2) middle(hk): 分发 + 起
    cm = _distribute("middle")
    _install_and_restart_role(cm,"middle",_node_command("middle"))
    print("middle(hk):", run(cm, "pgrep -x nb_node>/dev/null&&echo NB_MIDDLE_UP||echo DOWN; tail -3 /tmp/nb_middle.log"))
    # 3) entry(gz): 分发 + 起; route = 经 middle(kz地址) 到 exit, exit 连本地 http
    cg = _distribute("entry")
    route = f"H:{kz_ip}:4443,T:127.0.0.1:9000"
    sec=_remote_security("entry")
    entry_cmd=(f"{WORK}/nb_node -r entry -l 8080 -n {hk_ip} -N 4443 -R {shlex.quote(route)} "
               f"-c {sec['cert']} -k {sec['key']} -a {sec['ca']}")
    _install_and_restart_role(cg,"entry",entry_cmd)
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
    _require_security_material()
    gz = _role_host("entry")
    bindata = (BUILD_DIR / "nb_node").read_bytes()

    ck = connect("exit")
    _systemd_stop(ck,"exit")
    run(ck, f"mkdir -p {WORK}/logs")
    push_bytes(ck, bindata, f"{WORK}/nb_node", mode=0o755)
    _push_security(ck,"exit")
    _push_tiktok_rules(ck)
    wl_remote = _ensure_remote_whitelist(ck)
    print("exit(kz):", _install_and_restart_role(ck, "exit", _node_command("exit", wl_remote=wl_remote)))
    print(run(ck, f"tail -4 {WORK}/logs/nb-exit.log 2>/dev/null"))
    ck.close()

    cm = connect("middle")
    _systemd_stop(cm,"middle")
    push_bytes(cm, bindata, f"{WORK}/nb_node", mode=0o755)
    _push_security(cm,"middle")
    _push_tiktok_rules(cm)
    print("middle(hk):", _install_and_restart_role(cm, "middle", _node_command("middle")))
    print(run(cm, f"tail -4 {WORK}/logs/nb-middle.log 2>/dev/null"))
    cm.close()

    cg = connect("entry")
    _systemd_stop(cg,"entry")
    push_bytes(cg, bindata, f"{WORK}/nb_node", mode=0o755)
    _push_security(cg,"entry")
    _push_tiktok_rules(cg)
    _push_exit_routes(cg)
    entry_wl=_ensure_remote_whitelist(cg,role="entry")
    print("entry(gz):", _install_and_restart_role(cg, "entry", _node_command("entry", socks_port=socks_port,wl_remote=entry_wl)))
    print(run(cg, f"tail -4 {WORK}/logs/nb-entry.log 2>/dev/null"))
    cg.close()

    _smoke_socks(socks_port)
    print(f"\n>>> 手机配置: SOCKS5 -> {gz['host']}:{socks_port}，使用 NB_SOCKS_USERNAME 对应凭据")


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
    elif a.action == "fec-on": _set_fec_roles(True)
    elif a.action == "fec-off": _set_fec_roles(False)


if __name__ == "__main__":
    main()
