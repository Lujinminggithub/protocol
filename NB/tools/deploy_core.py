#!/usr/bin/env python3
"""Newbility(NB) 三跳部署工具 —— entry(gz) / middle(hk) / exit(kz)。

角色与机器由 lab-hosts.json 描述; middle 经 entry 跳板连接(direct-tcpip)。
NB 节点是一份二进制 nb_node, 靠 -r 选角色。生产构建固定在 Entry 的
compile_dir，Entry 向 Relay 分发，Exit 直传失败时由 Relay 中转；middle/exit 无需编译器。

动作:
  recon        只读侦察: 架构/gcc/picoquic/certs/现有 nb 进程, 不改动。
  build        上传源码到 Entry 构建目录并编译，下载一份校验缓存。
  deploy-tri   分发二进制 + 起 exit->middle->entry + 冒烟(多 stream + md5)。
  stop         停三跳所有 nb_node 进程。
  logs         拉三跳最近日志(便于跨跳追踪 route+stream_id)。

用法: python deploy.py <action> [--target host:port]
"""
from __future__ import annotations
import argparse, hashlib, io, ipaddress, json, pathlib, tarfile, time, sys, os, shlex, subprocess, logging, random
import paramiko
import nb_release
import nb_shard_deploy
import deploy_shard_runtime
import deploy_transfer

logging.getLogger("paramiko.transport").setLevel(logging.CRITICAL)

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parents[1]         # E:/project/NB
SRC = ROOT / "src"
LAB_FILE = pathlib.Path(os.environ.get("NB_HOSTS_FILE", str(ROOT / "tools" / "lab-hosts.json")))
LAB = json.loads(LAB_FILE.read_text(encoding="utf-8"))
WORK = LAB["paths"]["work_dir"]
DEPLOY_INSTANCE = os.environ.get("NB_DEPLOY_INSTANCE", "").strip()
_SAFE_INSTANCE = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
if (DEPLOY_INSTANCE and (len(DEPLOY_INSTANCE) > 48 or
        any(ch not in _SAFE_INSTANCE for ch in DEPLOY_INSTANCE))):
    raise ValueError("NB_DEPLOY_INSTANCE must use 1..48 safe identifier characters")
INSTANCE_WORK = f"{WORK}/instances/{DEPLOY_INSTANCE}" if DEPLOY_INSTANCE else WORK
COMPILE_WORK = LAB["paths"].get("compile_dir", "/opt/compile")
DEPLOY_CERTS = f"{INSTANCE_WORK}/certs"
BUILD_HOST = LAB.get("build_host", "entry")                 # 独立构建机角色，默认使用广州 entry
BUILD_DIR = ROOT / "build"
RELEASE_MANIFEST = BUILD_DIR / "release-manifest.json"
LINE_PROFILE = pathlib.Path(os.environ.get("NB_LINE_PROFILE_FILE", str(BUILD_DIR / "line-profiles" / "active.json")))
PLATFORM = "linux-x86_64"             # 目标平台(三跳均 x86_64)
SECURITY_DIR = pathlib.Path(os.environ.get("NB_SECURITY_DIR", str(BUILD_DIR / "security")))
DEFAULT_SOCKS_PORT = int(os.environ.get("NB_SOCKS_PORT", "1080"))
MIDDLE_PORT = int(os.environ.get("NB_MIDDLE_PORT", "4443"))
EXIT_PORT = int(os.environ.get("NB_EXIT_PORT", "4443"))
if (not 1 <= DEFAULT_SOCKS_PORT <= 65535 or not 1 <= MIDDLE_PORT <= 65535 or
        not 1 <= EXIT_PORT <= 65535):
    raise ValueError("NB_SOCKS_PORT, NB_MIDDLE_PORT and NB_EXIT_PORT must be valid ports")
WHITELIST_LOCAL = ROOT / "tools" / "whitelist.local.conf"
LOG4C_RUNTIME_CONFIG = ROOT / "tools" / "log4c.runtime.json"

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
domain_exact odr.itunes.apple.com
domain www.google.com
port 443
port 80
port 50000
port 50001
port 50008
port 50009
port 50020
port 50021
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


def _bind_jump_lifecycle(client: paramiko.SSHClient, jump: paramiko.SSHClient | None) -> None:
    """让目标连接关闭时同步关闭跳板，避免长期采集泄漏 sshd 会话。"""
    setattr(client, "_jump", jump)
    if jump is None:
        return
    original_close = client.close
    closed = False

    def close_with_jump() -> None:
        nonlocal closed
        if closed:
            return
        closed = True
        try:
            original_close()
        finally:
            jump.close()

    client.close = close_with_jump


def ssh_retry_delay(attempt: int, base: float = 1.0, cap: float = 12.0,
                    jitter: float | None = None) -> float:
    """Bounded exponential backoff; attempt is zero based."""
    value = min(cap, base * (2 ** max(0, attempt)))
    factor = random.uniform(0.75, 1.25) if jitter is None else jitter
    return max(0.0, min(cap, value * factor))


def _jump_strategies(role: str) -> list[str | None]:
    h = _role_host(role)
    policy = h.get("jump_policy", "pinned" if h.get("jump_via") else "direct")
    if policy == "direct":
        return [None]
    if policy == "pinned":
        return [h.get("jump_via")] if h.get("jump_via") else [None]
    if policy != "auto":
        raise ValueError(f"{role}.jump_policy must be direct, pinned or auto")
    values = [None, *h.get("jump_candidates", [])]
    if h.get("jump_via"):
        values.append(h["jump_via"])
    result = []
    for value in values:
        if value != role and value not in result:
            result.append(value)
    return result


def _connect(role: str, stack: tuple[str, ...]) -> paramiko.SSHClient:
    """Try direct and declared SSH jumps, preserving nested jump lifetimes."""
    if role in stack:
        raise RuntimeError(f"SSH jump cycle: {' -> '.join((*stack, role))}")
    h = _role_host(role)
    last = None
    failures = []
    max_attempts = max(1, min(10, int(os.environ.get("NB_SSH_CONNECT_ATTEMPTS", "5"))))
    for attempt in range(max_attempts):
        for jump_role in _jump_strategies(role):
            c = None
            jump = None
            try:
                sock = None
                if jump_role:
                    if jump_role not in LAB:
                        raise ValueError(f"unknown SSH jump role: {jump_role}")
                    jump = _connect(jump_role, (*stack, role))
                    target = h.get("jump_target_host") or h.get("private_ip") or h["host"]
                    sock = jump.get_transport().open_channel(
                        "direct-tcpip", (target, h["port"]), ("127.0.0.1", 0))
                c = paramiko.SSHClient(); _configure_host_keys(c)
                c.connect(hostname=h["host"], port=h["port"], username=h["user"],
                          password=_host_password(h), timeout=25, banner_timeout=25,
                          auth_timeout=25, allow_agent=False, look_for_keys=False, sock=sock)
                _bind_jump_lifecycle(c, jump)
                return c
            except Exception as e:  # noqa
                last = e
                strategy = f"jump:{jump_role}" if jump_role else "direct"
                failures.append(f"attempt={attempt + 1} strategy={strategy} "
                                f"error={type(e).__name__}: {str(e)[:240]}")
                if c is not None:
                    try: c.close()
                    except Exception: pass
                if jump is not None:
                    try: jump.close()
                    except Exception: pass
        if attempt + 1 < max_attempts:
            time.sleep(ssh_retry_delay(attempt))
    detail = "; ".join(failures[-6:]) or f"{type(last).__name__}: {last}"
    raise RuntimeError(f"connect {role}({h['name']}) failed after retries: {detail}")


def connect(role) -> paramiko.SSHClient:
    """Connect using the line's bounded direct/jump strategy list."""
    return _connect(role, ())


def run(c, cmd, tmo=120):
    _i, o, e = c.exec_command(cmd, timeout=tmo)
    return o.read().decode("utf-8", "replace") + e.read().decode("utf-8", "replace")


def checked_run(c, cmd, tmo=120):
    _i, o, e = c.exec_command(cmd, timeout=tmo)
    stdout = o.read().decode("utf-8", "replace")
    stderr = e.read().decode("utf-8", "replace")
    status = o.channel.recv_exit_status()
    if status != 0:
        detail = (stdout + stderr).strip()
        raise RuntimeError(f"remote command failed rc={status}: {detail[-4096:]}")
    return stdout + stderr


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
    """Resume an interrupted SFTP transfer and atomically publish verified bytes."""
    digest = hashlib.sha256(data).hexdigest()
    tmp = f"{path}.part.{digest[:16]}"
    run(c, f"mkdir -p $(dirname {shlex.quote(path)})")
    for verify_attempt in range(2):
        sf = None
        try:
            sf = c.open_sftp()
            try:
                offset = sf.stat(tmp).st_size
            except OSError:
                offset = 0
            if offset > len(data):
                sf.remove(tmp); offset = 0
            if offset < len(data):
                with sf.file(tmp, "ab") as stream:
                    for start in range(offset, len(data), 1024 * 1024):
                        stream.write(data[start:start + 1024 * 1024])
                    stream.flush()
            sf.chmod(tmp, mode)
        finally:
            if sf is not None:
                sf.close()
        remote_digest = run(c, f"sha256sum {shlex.quote(tmp)} | awk '{{print $1}}'").strip()
        if remote_digest == digest:
            run(c, f"mv -f {shlex.quote(tmp)} {shlex.quote(path)}")
            return
        run(c, f"rm -f {shlex.quote(tmp)}")
    raise RuntimeError(f"resumable upload checksum mismatch: {path}")


def _effective_workers(role):
    workers = int(LAB.get("workers", {}).get(role, 1))
    if not 1 <= workers <= 32:
        raise ValueError(f"workers.{role} 必须为 1..32")
    return workers


def _service_name(role):
    return f"nb-{DEPLOY_INSTANCE}-{role}" if DEPLOY_INSTANCE else f"nb-{role}"


def _control_socket_prefix(role):
    return _service_name(role)


def _control_socket_path(role, worker):
    return f"/run/{_control_socket_prefix(role)}-{worker}.ctl"


def _control_socket_glob(role):
    return f"/run/{_control_socket_prefix(role)}-*.ctl"


def _log_path(role):
    return f"{INSTANCE_WORK}/logs/{_service_name(role)}.log"


def _acquire_deploy_lock(c, role, release_id):
    lock = "/run/nb-deploy.lock"
    output = run(c,
        f"if test -d {lock} && find {lock} -mmin +30 -print -quit | grep -q .; then rm -rf {lock}; fi; "
        f"if mkdir {lock} 2>/dev/null; then printf '%s %s UTC=%s\n' {shlex.quote(release_id)} {shlex.quote(role)} \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\" >{lock}/owner; echo LOCKED; "
        f"else echo BUSY; cat {lock}/owner 2>/dev/null || true; fi")
    if "LOCKED" not in output:
        raise RuntimeError(f"{role} 已有部署事务执行中: {output.strip()}")


def _release_deploy_lock(c):
    run(c, "rm -rf /run/nb-deploy.lock")


def _deployment_id(manifest):
    return manifest.get("deployment_id") or manifest["release_id"]


def _append_deploy_audit(c, role, event, manifest, previous=None, detail=None):
    record = {
        "at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "event": event,
        "role": role,
        "host": _role_host(role)["name"],
        "release_id": manifest["release_id"],
        "deployment_id": _deployment_id(manifest),
        "artifact_sha256": manifest["artifact"]["sha256"],
        "previous": previous,
        "detail": detail,
        "instance": DEPLOY_INSTANCE or "legacy",
    }
    line = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    with (BUILD_DIR / "deploy-audit.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(line + "\n")
    if c is not None:
        remote = f"{INSTANCE_WORK}/releases/deploy-audit.jsonl"
        run(c, f"mkdir -p {shlex.quote(INSTANCE_WORK + '/releases')}; printf '%s\\n' {shlex.quote(line)} >> {shlex.quote(remote)}; chmod 0600 {shlex.quote(remote)}")


def _append_exact_rollback_audit(c,role,event,target,origin,detail=None):
    record={"at_utc":time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime()),"event":event,
            "role":role,"host":_role_host(role)["name"],"target_deployment_id":target,
            "origin_deployment_id":origin,"detail":detail,"instance":DEPLOY_INSTANCE or "legacy"}
    line=json.dumps(record,ensure_ascii=False,separators=(",",":"));BUILD_DIR.mkdir(parents=True,exist_ok=True)
    with (BUILD_DIR/"deploy-audit.jsonl").open("a",encoding="utf-8") as stream:stream.write(line+"\n")
    remote=f"{INSTANCE_WORK}/releases/deploy-audit.jsonl"
    run(c,f"printf '%s\\n' {shlex.quote(line)} >> {shlex.quote(remote)}; chmod 0600 {shlex.quote(remote)}")


def _prune_releases(c, role, previous):
    retain = int(LAB.get("release_retention", 5))
    if not 2 <= retain <= 20:
        raise ValueError("release_retention 必须为 2..20")
    previous_name = pathlib.PurePosixPath(previous).parent.name if previous != "NONE" else ""
    script = (
        "import json,pathlib,re,shutil;"
        f"root=pathlib.Path({str(INSTANCE_WORK + '/releases')!r}).resolve();"
        f"retain={retain};protected={{{previous_name!r}}};"
        f"current=pathlib.Path({str(INSTANCE_WORK + '/nb_node')!r}).resolve().parent.name;protected.add(current);"
        "rx=re.compile(r'^(?:[0-9a-f]{16}(?:-[0-9a-f]{12})?|legacy-[0-9a-f]{16})$');"
        "items=sorted(((p.stat().st_mtime,p) for p in root.iterdir() if p.is_dir() and rx.fullmatch(p.name)),reverse=True);"
        "keep={p.name for _,p in items[:retain]}|protected;removed=[];"
        "[(shutil.rmtree(p),removed.append(p.name)) for _,p in items if p.name not in keep and p.parent.resolve()==root];"
        "print(json.dumps({'removed':removed,'kept':sorted(keep)}))"
    )
    output = run(c, f"python3 -c {shlex.quote(script)}", tmo=30).strip()
    if not output.startswith("{"):
        raise RuntimeError(f"{role} 发布保留清理失败: {output}")
    return output


def _line_profile_identity():
    if not LINE_PROFILE.is_file():
        return "unversioned", 0
    profile = json.loads(LINE_PROFILE.read_text(encoding="utf-8"))
    line_id = str(profile.get("line_id", "unversioned"))
    schema = int(profile.get("schema_version", 0))
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-"
    if not line_id or any(ch not in allowed for ch in line_id) or not 0 <= schema <= 999999:
        raise ValueError("线路 profile 的 line_id/schema_version 非法")
    return line_id, schema


def _stage_release(c, role, manifest, binary=None):
    release_id = _deployment_id(manifest)
    if not nb_release.RELEASE_NAME_RE.fullmatch(release_id) or release_id.startswith("legacy-"):
        raise ValueError(f"非法 deployment_id: {release_id}")
    release_dir = f"{INSTANCE_WORK}/releases/{release_id}"
    run(c, f"mkdir -p {shlex.quote(release_dir)}")
    expected = manifest["artifact"]["sha256"]
    artifact = f"{release_dir}/nb_node"
    existing = run(c, f"test -f {shlex.quote(artifact)} && sha256sum {shlex.quote(artifact)} | awk '{{print $1}}' || true").strip()
    if existing and existing != expected:
        raise RuntimeError(f"{role} immutable release collision: {release_id}")
    if not existing and binary is None:
        raise RuntimeError(f"{role} release artifact was not distributed: {release_id}")
    if not existing:
        push_bytes(c, binary, artifact, mode=0o755)
    push_bytes(c, RELEASE_MANIFEST.read_bytes(), f"{release_dir}/release-manifest.json", mode=0o644)
    log_config = json.loads(LOG4C_RUNTIME_CONFIG.read_text(encoding="utf-8"))
    log_config["log_dir"] = f"{INSTANCE_WORK}/logs"
    push_bytes(c, (json.dumps(log_config, separators=(",", ":")) + "\n").encode("utf-8"),
               f"{release_dir}/cfg/log4c.json", mode=0o644)
    verified = run(c, f"test \"$(sha256sum {shlex.quote(release_dir + '/nb_node')} | awk '{{print $1}}')\" = {shlex.quote(expected)} && echo VERIFIED")
    if "VERIFIED" not in verified:
        raise RuntimeError(f"{role} 发布产物远端哈希校验失败")

    current = f"{INSTANCE_WORK}/nb_node"
    previous = run(c,
        f"if test -L {shlex.quote(current)}; then echo PREVIOUS=$(readlink {shlex.quote(current)}); "
        f"elif test -f {shlex.quote(current)}; then old=$(sha256sum {shlex.quote(current)} | awk '{{print $1}}'); "
        f"short=$(printf '%s' \"$old\" | cut -c1-16); olddir={shlex.quote(INSTANCE_WORK)}/releases/legacy-$short; "
        f"mkdir -p \"$olddir\"; cp -p {shlex.quote(current)} \"$olddir/nb_node\"; echo PREVIOUS=releases/legacy-$short/nb_node; "
        "else echo PREVIOUS=NONE; fi").strip().splitlines()
    marker = next((line for line in reversed(previous) if line.startswith("PREVIOUS=")), None)
    if marker is None:
        raise RuntimeError(f"{role} 无法保存当前发布指针")
    return marker.split("=", 1)[1]


def _copy_release_between_nodes(source_c, source_role, target_c, target_role, manifest):
    return deploy_transfer.copy_release(
        source_c, source_role, target_c, target_role, manifest,
        instance_work=INSTANCE_WORK, role_host=_role_host, run=run,
        checked_run=checked_run, push_bytes=push_bytes)


def _activate_release(c, release_id):
    target = f"releases/{release_id}/nb_node"
    temporary = f"{INSTANCE_WORK}/.nb_node.next"
    output = run(c, f"ln -sfn {shlex.quote(target)} {shlex.quote(temporary)} && "
                    f"mv -Tf {shlex.quote(temporary)} {shlex.quote(INSTANCE_WORK + '/nb_node')} && echo ACTIVATED")
    if "ACTIVATED" not in output:
        raise RuntimeError(f"远端原子切换失败: release={release_id}")


def _backup_role_unit(c, role, release_id):
    service = _service_name(role)
    unit = f"/etc/systemd/system/{service}.service"
    backup = f"{INSTANCE_WORK}/releases/{release_id}/previous-{service}.service"
    output = run(c, f"if test -f {shlex.quote(unit)}; then cp -p {shlex.quote(unit)} {shlex.quote(backup)}; "
                    "echo UNIT_BACKED_UP; else echo UNIT_ABSENT; fi")
    if "UNIT_BACKED_UP" in output:
        return True
    if "UNIT_ABSENT" in output:
        return False
    raise RuntimeError(f"{role} 无法备份当前 systemd unit")


def _mutable_role_paths(role):
    paths = [
        f"{DEPLOY_CERTS}/ca.pem",
        f"{DEPLOY_CERTS}/{role}.pem",
        f"{DEPLOY_CERTS}/{role}.key",
    ]
    if role == "entry":
        paths += [f"{INSTANCE_WORK}/socks.users", f"{INSTANCE_WORK}/tenant.conf", _exit_routes_remote()]
    if DEPLOY_INSTANCE:
        shard_root=nb_shard_deploy.shard_root(WORK)
        paths += [f"{shard_root}/nb_node",f"/etc/systemd/system/nb-{role}-shard@.service"]
        paths += [f"{nb_shard_deploy.shard_config_dir(WORK,role,worker)}/{DEPLOY_INSTANCE}.conf"
                  for worker in range(_effective_workers(role))]
    return paths


def _backup_role_state(c, role, release_id):
    root = f"{INSTANCE_WORK}/releases/{release_id}/previous-state"
    run(c, f"mkdir -p {shlex.quote(root)}")
    result = {}
    for index, path in enumerate(_mutable_role_paths(role)):
        backup = f"{root}/{index}"
        output = run(c, f"if test -e {shlex.quote(path)} || test -L {shlex.quote(path)}; then cp -Pp {shlex.quote(path)} {shlex.quote(backup)} && "
                        "echo PRESENT; else echo ABSENT; fi")
        if "PRESENT" in output:
            result[path] = True
        elif "ABSENT" in output:
            result[path] = False
        else:
            raise RuntimeError(f"{role} mutable state backup failed: {path}")
    return result


def _restore_role_state(c, role, release_id, state):
    root = f"{INSTANCE_WORK}/releases/{release_id}/previous-state"
    expected = _mutable_role_paths(role)
    if set(state) != set(expected):
        raise RuntimeError(f"{role} mutable state backup manifest mismatch")
    for index, path in enumerate(expected):
        if state[path]:
            run(c, f"mkdir -p $(dirname {shlex.quote(path)}) && cp -Pp {shlex.quote(f'{root}/{index}')} {shlex.quote(path)}")
        else:
            run(c, f"rm -f {shlex.quote(path)}")


def _rollback_release(c, role, previous, release_id, had_unit, mutable_state=None):
    service = _service_name(role)
    unit = f"/etc/systemd/system/{service}.service"
    backup = f"{INSTANCE_WORK}/releases/{release_id}/previous-{service}.service"
    if mutable_state is not None:
        _restore_role_state(c, role, release_id, mutable_state)
    if DEPLOY_INSTANCE:
        workers=_effective_workers(role)
        run(c,"systemctl daemon-reload")
        unit_exists=run(c,f"test -f /etc/systemd/system/nb-{role}-shard@.service && echo yes || echo no").strip()=="yes"
        for worker in range(workers):
            shard_service=nb_shard_deploy.shard_service(role,worker)
            if unit_exists:run(c,f"systemctl restart {shlex.quote(shard_service)}; sleep 1")
            else:run(c,f"systemctl stop {shlex.quote(shard_service)} 2>/dev/null || true")
        if had_unit:
            restored = run(c, f"cp -p {shlex.quote(backup)} {shlex.quote(unit)} && systemctl daemon-reload && systemctl start {shlex.quote(service)} && echo UNIT_RESTORED")
            if "UNIT_RESTORED" not in restored:raise RuntimeError(f"{role} legacy unit rollback failed")
        elif unit_exists:
            deploy_shard_runtime.verify_all_controls(c, role, work=WORK, run=run)
        if previous!="NONE":
            temporary=f"{INSTANCE_WORK}/.nb_node.rollback"
            run(c,f"ln -sfn {shlex.quote(previous)} {shlex.quote(temporary)} && mv -Tf {shlex.quote(temporary)} {shlex.quote(INSTANCE_WORK + '/nb_node')}")
        return f"shard rollback workers={workers} controls=ok"
    if had_unit:
        restored = run(c, f"cp -p {shlex.quote(backup)} {shlex.quote(unit)} && systemctl daemon-reload && echo UNIT_RESTORED")
        if "UNIT_RESTORED" not in restored:
            raise RuntimeError(f"{role} systemd unit 回滚失败")
    else:
        run(c, f"rm -f {shlex.quote(unit)} && systemctl daemon-reload")
    if previous == "NONE":
        _systemd_stop(c, role)
        run(c, f"rm -f {shlex.quote(INSTANCE_WORK + '/nb_node')}")
        return "stopped(no previous release)"
    temporary = f"{INSTANCE_WORK}/.nb_node.rollback"
    run(c, f"ln -sfn {shlex.quote(previous)} {shlex.quote(temporary)} && "
           f"mv -Tf {shlex.quote(temporary)} {shlex.quote(INSTANCE_WORK + '/nb_node')}")
    return _systemd_restart(c, role, warmup=2.0)


def _verify_release_health(c, role, manifest, warmup=8):
    expected_hash = manifest["artifact"]["sha256"]
    result = _verify_deployment_health(c, role, _deployment_id(manifest), warmup, expected_hash)
    return result


def _verify_deployment_health(c, role, deployment_id, warmup=8, expected_hash=None):
    expected_workers = _effective_workers(role)
    time.sleep(warmup)
    service = _service_name(role)
    if DEPLOY_INSTANCE:
        active="; ".join(f"systemctl is-active {shlex.quote(nb_shard_deploy.shard_service(role,worker))}"
                         for worker in range(expected_workers))
        binary=nb_shard_deploy.shard_root(WORK)+"/nb_node"
        state=run(c,f"{active}; sha256sum {shlex.quote(binary)} | awk '{{print $1}}'").strip().splitlines()
        active_count=sum(item=="active" for item in state)
    else:
        state = run(c, f"systemctl is-active {shlex.quote(service)}; "
                       f"sha256sum {shlex.quote(INSTANCE_WORK + '/nb_node')} | awk '{{print $1}}'").strip().splitlines()
        active_count=sum(item=="active" for item in state)
    if active_count<expected_workers or (expected_hash is not None and expected_hash not in state):
        raise RuntimeError(f"nb-{role} systemd/哈希健康门禁失败: {' '.join(state)}")
    script = (
        "import glob,json,socket,sys;"
        f"paths={[_control_socket_path(role, worker) for worker in range(expected_workers)]!r};"
        "responses=[];"
        "[(lambda s,p:(s.settimeout(2),s.connect(p),s.sendall(b'health\\n'),responses.append(json.loads(s.recv(2048).decode())),s.close()))(socket.socket(socket.AF_UNIX),p) for p in paths];"
        f"assert all(x.get('status')=='ok' and x.get('role')=='{role}' and x.get('release_id')=='{deployment_id}' for x in responses),responses;"
        "print('HEALTH_OK')"
    )
    health = run(c, f"python3 -c {shlex.quote(script)}", tmo=15)
    if "HEALTH_OK" not in health:
        raise RuntimeError(f"nb-{role} 控制套接字健康门禁失败: {health.strip()}")
    return f"deployment={deployment_id} workers={expected_workers} health=ok"


def _remote_current_deployment(c, role):
    output = run(c, f"readlink -f {shlex.quote(INSTANCE_WORK + '/nb_node')} 2>/dev/null | xargs -r dirname | xargs -r basename").strip()
    if not nb_release.RELEASE_NAME_RE.fullmatch(output) or output.startswith("legacy-"):
        raise RuntimeError(f"{role} 当前 deployment 无法识别: {output or 'missing'}")
    return output


def _service_exists(c, role):
    if DEPLOY_INSTANCE:
        return all(run(c,f"systemctl cat {shlex.quote(nb_shard_deploy.shard_service(role,worker))} >/dev/null 2>&1 && echo yes || echo no").strip()=="yes"
                   for worker in range(_effective_workers(role)))
    unit = _service_name(role)
    return run(c, f"systemctl cat {unit} >/dev/null 2>&1 && echo yes || echo no").strip() == "yes"


def _systemd_restart(c, role, warmup=2.0):
    if DEPLOY_INSTANCE:
        states=[]
        for worker in range(_effective_workers(role)):
            unit=nb_shard_deploy.shard_service(role,worker)
            states.extend(run(c,f"systemctl restart {shlex.quote(unit)}; sleep {warmup}; systemctl is-active {shlex.quote(unit)}; systemctl show -p MainPID --value {shlex.quote(unit)}").strip().splitlines())
        return " ".join(x.strip() for x in states if x.strip())
    unit = _service_name(role)
    mainpid = run(c, f"systemctl restart {unit}; sleep {warmup}; systemctl is-active {unit}; "
                     f"systemctl show -p MainPID --value {unit}").strip().splitlines()
    return " ".join(x.strip() for x in mainpid if x.strip())


def _systemd_stop(c, role):
    if DEPLOY_INSTANCE:
        paths=[f"{nb_shard_deploy.shard_config_dir(WORK,role,worker)}/{DEPLOY_INSTANCE}.conf"
               for worker in range(_effective_workers(role))]
        controls=[nb_shard_deploy.line_control_path(DEPLOY_INSTANCE,role,worker)
                  for worker in range(_effective_workers(role))]
        run(c,"rm -f "+" ".join(shlex.quote(path) for path in paths))
        for worker in range(_effective_workers(role)):
            run(c,f"systemctl reload {shlex.quote(nb_shard_deploy.shard_service(role,worker))} 2>/dev/null || true")
        run(c,f"systemctl stop {shlex.quote(_service_name(role))} 2>/dev/null || true")
        absent=" && ".join([*(f"test ! -e {shlex.quote(path)}" for path in paths),
                            *(f"test ! -S {shlex.quote(path)}" for path in controls)])
        details=" ".join(shlex.quote(path) for path in [*paths,*controls])
        result=run(c,f"for i in $(seq 1 40); do if {absent}; then echo LINE_INSTANCE_REMOVED; exit 0; fi; sleep 0.25; done; "
                     f"ls -l {details} 2>/dev/null || true; exit 1",tmo=20)
        if "LINE_INSTANCE_REMOVED" not in result:
            raise RuntimeError(f"{role} line instance cleanup verification failed")
        return "line instance removed; shard remains active"
    unit = _service_name(role)
    cleanup = (f"rm -f {_control_socket_glob(role)}; " if DEPLOY_INSTANCE else
               "pkill -9 -x nb_node 2>/dev/null || true; ")
    return run(c, f"systemctl stop {unit} 2>/dev/null || true; {cleanup}echo stopped")


def _require_local_build():
    if not (BUILD_DIR / "nb_node").exists():
        sys.exit("本地缺少 build/nb_node，请先运行: python tools/deploy.py build")
    if not RELEASE_MANIFEST.is_file():
        sys.exit("本地缺少 build/release-manifest.json，请重新运行 build")
    try:
        return nb_release.load_and_validate_manifest(
            RELEASE_MANIFEST, ROOT, BUILD_DIR / "nb_node", LAB_FILE,
            LINE_PROFILE if LINE_PROFILE.is_file() else None,
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        sys.exit(f"发布清单校验失败: {error}")


def _require_security_material():
    required = [SECURITY_DIR / "ca.pem", SECURITY_DIR / "socks.users", SECURITY_DIR / "tenant.conf"]
    required += [SECURITY_DIR / f"{role}.{ext}" for role in ("entry", "middle", "exit") for ext in ("pem", "key")]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        sys.exit("缺少安全材料，请先运行 tools/security_setup.py:\n" + "\n".join(missing))


def _remote_security(role):
    return {
        "ca": f"{DEPLOY_CERTS}/ca.pem",
        "cert": f"{DEPLOY_CERTS}/{role}.pem",
        "key": f"{DEPLOY_CERTS}/{role}.key",
        "users": f"{INSTANCE_WORK}/socks.users",
        "tenants": f"{INSTANCE_WORK}/tenant.conf",
    }


def _push_security(c, role):
    _require_security_material()
    paths = _remote_security(role)
    push_bytes(c, (SECURITY_DIR / "ca.pem").read_bytes(), paths["ca"], mode=0o644)
    push_bytes(c, (SECURITY_DIR / f"{role}.pem").read_bytes(), paths["cert"], mode=0o644)
    push_bytes(c, (SECURITY_DIR / f"{role}.key").read_bytes(), paths["key"], mode=0o600)
    if role == "entry":
        push_bytes(c, (SECURITY_DIR / "socks.users").read_bytes(), paths["users"], mode=0o600)
    if role in ("entry", "exit"):
        existing = run(c, f"test -f {shlex.quote(paths['tenants'])} && echo PRESENT || true").strip()
        if "PRESENT" not in existing:
            push_bytes(c, (SECURITY_DIR / "tenant.conf").read_bytes(), paths["tenants"], mode=0o600)


def _whitelist_remote():
    return f"{INSTANCE_WORK}/whitelist.conf"


def _tiktok_rules_remote():
    return f"{INSTANCE_WORK}/tiktok_flow_rules.conf"


def _exit_routes_remote():
    return f"{INSTANCE_WORK}/exit_routes.conf"


def _push_exit_routes(c, remote_path=None):
    exits=LAB.get("exits") or [{"name":_role_host("exit")["name"],"host":_role_host("exit")["host"],"port":4443,"weight":1}]
    lines=["# route <name> <H:host:port> <weight> <capacity; 0=unlimited>"]
    for item in exits:
        name=str(item["name"]);host=str(item["host"]);port=int(item.get("port",4443));weight=int(item.get("weight",1))
        if item.get("fixed_exit",_role_host("exit")["name"])!=_role_host("exit")["name"]:
            raise ValueError(f"出口路由越过固定 exit: {item}")
        if host!=str(_role_host("exit")["host"]) or port!=EXIT_PORT:
            raise ValueError(f"exit route/listener drift: route={host}:{port} listener={_role_host('exit')['host']}:{EXIT_PORT}")
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", name) or not 1<=port<=65535 or not 1<=weight<=1000:
            raise ValueError(f"非法出口路由配置: {item}")
        capacity=int(item.get("capacity",0))
        if not 0<=capacity<=100000: raise ValueError(f"非法出口容量: {item}")
        lines.append(f"route {name} H:{host}:{port} {weight} {capacity}")
    destination = remote_path or _exit_routes_remote()
    push_bytes(c,("\n".join(lines)+"\n").encode("ascii"),destination,mode=0o644)
    return destination

def _ensure_remote_whitelist(c, role="exit"):
    wl_remote = _whitelist_remote()
    exists = run(c, f"test -f {wl_remote} && echo EXISTS || echo NONE").strip()
    if "EXISTS" not in exists:
        push_bytes(c, WL_DEFAULT.encode("utf-8"), wl_remote)
        print(f"whitelist: 内置默认模板已推送 -> {role}:{wl_remote}(后续远程编辑此文件即可)")
    else:
        print(f"whitelist: {role} 已有 {wl_remote}(保留远程配置, 不覆盖)")
    return wl_remote


def _push_whitelist(c, local_path: pathlib.Path, role="exit"):
    if not local_path.exists():
        sys.exit(f"白名单文件不存在: {local_path}")
    wl_remote = _whitelist_remote()
    push_bytes(c, local_path.read_bytes(), wl_remote)
    print(f"whitelist: 已下发 {local_path} -> {role}:{wl_remote}")
    return wl_remote


def _push_tiktok_rules(c, remote_path=None):
    local_rules = ROOT / "tools" / "tiktok_flow_rules.conf"
    if local_rules.exists():
        push_bytes(c, local_rules.read_bytes(), remote_path or _tiktok_rules_remote())


def _fec_override_path(role):
    return f"/etc/systemd/system/{_service_name(role)}.service.d/override.conf"


def _exit_bind_ip():
    value = str(_role_host("exit").get("outip") or "").strip()
    if not value:
        return ""
    try:
        address = ipaddress.ip_address(value)
    except ValueError as error:
        raise ValueError("exit outip must be a valid IPv4 address") from error
    if address.version != 4:
        raise ValueError("exit outip must be an IPv4 address")
    return str(address)


def _verify_exit_bind_ip(c):
    value = _exit_bind_ip()
    if not value:
        return "default-route"
    command = ("ip -4 -o addr show | awk '{print $4}' | cut -d/ -f1 | "
               f"grep -Fx -- {shlex.quote(value)} >/dev/null && echo BIND_IP_OK")
    if "BIND_IP_OK" not in run(c, command, tmo=15):
        raise RuntimeError(f"configured exit IP is not assigned on the exit device: {value}")
    return value


def _role_fec_enabled(c, role):
    if _service_exists(c, role):
        out = run(c, f"systemctl show -p Environment --value {_service_name(role)} 2>/dev/null")
        return "NB_FEC_V15_ACTIVE=on" in out or "NB_FEC_V15_ACTIVE=1" in out
    out = run(c,
        "pid=$(pgrep -xo nb_node 2>/dev/null || true); "
        "if [ -n \"$pid\" ] && [ -r /proc/$pid/environ ]; then "
        "tr '\\0' '\\n' </proc/$pid/environ | grep '^NB_FEC_V15_ACTIVE=' || true; fi")
    return "NB_FEC_V15_ACTIVE=on" in out or "NB_FEC_V15_ACTIVE=1" in out


def _node_command(role, socks_port=DEFAULT_SOCKS_PORT, wl_remote=None, release_id=None, exit_routes=None):
    hk = _role_host("middle")
    kz = _role_host("exit")
    sec = _remote_security(role)
    rules = f"{INSTANCE_WORK}/releases/{release_id}/tiktok_flow_rules.conf" if release_id else _tiktok_rules_remote()
    base = f"{INSTANCE_WORK}/nb_node -r {role} -c {sec['cert']} -k {sec['key']} -a {sec['ca']} -F {rules}"
    if role == "entry":
        signal_direct=LAB.get("transport",{}).get("entry",{}).get("signal_direct",False)
        if not isinstance(signal_direct,bool):
            raise ValueError("transport.entry.signal_direct 必须为 true 或 false")
        signal_direct_arg=" --signal-direct" if signal_direct else ""
        mid = f"H:{kz['host']}:{EXIT_PORT}"
        middle_data_host = hk.get("private_ip") or hk.get("jump_target_host") or hk["host"]
        if not wl_remote:
            raise ValueError("entry SOCKS 启动需要 whitelist 路径")
        return f"{base} -l {socks_port} -n {middle_data_host} -N {MIDDLE_PORT} -S -U {sec['users']} -Q {sec['tenants']} -W {wl_remote} -M {shlex.quote(mid)} -E {exit_routes or _exit_routes_remote()}{signal_direct_arg}"
    if role == "middle":
        return f"{base} -p {MIDDLE_PORT}"
    if role == "exit":
        if not wl_remote:
            raise ValueError("exit 启动需要 whitelist 路径")
        outip=_exit_bind_ip()
        source=f" -o {outip}" if outip else ""
        return f"{base} -p {EXIT_PORT} -Q {sec['tenants']} -W {wl_remote}{source}"
    raise ValueError(f"unknown role: {role}")


def _install_shard_role(c,role,command,environment,release_id,binary_release_id,warmup):
    return deploy_shard_runtime.install_role(
        c, role, command, environment, release_id, binary_release_id, warmup,
        work=WORK, instance_work=INSTANCE_WORK, deploy_instance=DEPLOY_INSTANCE,
        lab=LAB, run=run, push_bytes=push_bytes, effective_workers=_effective_workers,
        legacy_service_name=_service_name)


def _install_and_restart_role(c, role, command, warmup=2.0, release_id=None,binary_release_id=None):
    workers = _effective_workers(role)
    supervisor = f"{INSTANCE_WORK}/releases/{release_id}/nb_supervisor.py" if release_id else f"{INSTANCE_WORK}/nb_supervisor.py"
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
    mtu_max=int(transport.get("mtu_max",0))
    if mtu_max != 0 and not 1280<=mtu_max<=1536:
        raise ValueError(f"transport.{role}.mtu_max 必须为 0 或 1280..1536")
    mtu_env=(f"Environment=NB_MTU_MAX={mtu_max}\n" if mtu_max else "")
    udp_gso=transport.get("udp_gso",False)
    if not isinstance(udp_gso,bool):
        raise ValueError(f"transport.{role}.udp_gso 必须为 true 或 false")
    udp_gso_env=f"Environment=NB_UDP_GSO={'on' if udp_gso else 'off'}\n"
    dns_servers=[]
    dns_env=""
    if role=="exit":
        configured_dns=transport.get("dns_servers",["1.1.1.1","8.8.8.8"])
        if not isinstance(configured_dns,list) or not 1<=len(configured_dns)<=3:
            raise ValueError("transport.exit.dns_servers 必须包含 1 到 3 个 IPv4 地址")
        try:
            dns_servers=[str(ipaddress.IPv4Address(value)) for value in configured_dns]
        except (ipaddress.AddressValueError,TypeError) as exc:
            raise ValueError("transport.exit.dns_servers 必须是 IPv4 地址") from exc
        dns_env=f"Environment=NB_DNS_SERVERS={','.join(dns_servers)}\n"
    release_env = f"Environment=NB_RELEASE_ID={release_id}\n" if release_id else ""
    line_id, line_schema = _line_profile_identity()
    profile_env = (f"Environment=NB_LINE_PROFILE_ID={line_id}\n"
                   f"Environment=NB_LINE_PROFILE_SCHEMA={line_schema}\n")
    instance_env = (f"Environment=NB_INSTANCE_ID={DEPLOY_INSTANCE}\n"
                    f"Environment=NB_CONTROL_PREFIX={_control_socket_prefix(role)}\n")
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
        udp_port_min=int(os.environ.get("NB_SOCKS_UDP_PORT_MIN") or transport.get("udp_port_min",0))
        udp_port_max=int(os.environ.get("NB_SOCKS_UDP_PORT_MAX") or transport.get("udp_port_max",0))
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
    if DEPLOY_INSTANCE:
        environment={"NB_FEC_V15":"on","NB_CC":cc,"NB_BBR_OPTIONS":bbr_options,
            "NB_UDP_GSO":"on" if udp_gso else "off","NB_RELEASE_ID":release_id or "unversioned",
            "NB_LINE_PROFILE_ID":line_id,"NB_LINE_PROFILE_SCHEMA":str(line_schema),
            "NB_TRANSPORT_LINE_ID":line_id,
            "NB_TRANSPORT_PROFILE_FILE":f"{INSTANCE_WORK}/transport-profiles/{role}-active.conf"}
        if cwin_max_bytes:environment["NB_CWIN_MAX_BYTES"]=str(cwin_max_bytes)
        if mtu_max:environment["NB_MTU_MAX"]=str(mtu_max)
        if role=="exit":environment["NB_DNS_SERVERS"]=",".join(dns_servers)
        if role=="entry":
            if udp_advertise_ip:environment["NB_SOCKS_UDP_ADVERTISE_IP"]=udp_advertise_ip
            if udp_port_min:
                environment["NB_SOCKS_UDP_PORT_MIN"]=str(udp_port_min)
                environment["NB_SOCKS_UDP_PORT_MAX"]=str(udp_port_max)
        if role in ("entry","middle"):
            environment["NB_REORDER_GAP"]=str(reorder_gap)
            environment["NB_REORDER_DELAY_US"]=str(reorder_delay_us)
        return _install_shard_role(c,role,command,environment,release_id,binary_release_id or release_id,warmup)
    unit = f"""[Unit]
Description=Newbility {role} node
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=simple
Environment=MALLOC_ARENA_MAX=2
Environment=NB_FEC_V15=on
Environment=NB_WORKER_LANE_PORTS=on
Environment=NB_CC={cc}
Environment=NB_BBR_OPTIONS={bbr_options}
{cwin_env}{mtu_env}{udp_gso_env}{release_env}{profile_env}{instance_env}{udp_advertise_env}{reorder_env}{dns_env}ExecStart={exec_start}
Restart=on-failure
RestartSec=2
KillMode=control-group
LimitNOFILE=1048576
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
"""
    service = _service_name(role)
    remote = f"/etc/systemd/system/{service}.service"
    push_bytes(c, unit.encode("utf-8"), remote, mode=0o644)
    if release_id:
        run(c, f"cp -p {shlex.quote(remote)} {shlex.quote(f'{INSTANCE_WORK}/releases/{release_id}/{service}.service')}")
    run(c, f"systemctl daemon-reload && systemctl enable {service} >/dev/null")
    state = _systemd_restart(c, role, warmup=warmup)
    if "active" not in state:
        detail = run(c, f"systemctl status {service} --no-pager -l; journalctl -u {service} -n 30 --no-pager")
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
        tail = run(c, f"tail -8 {_log_path(role)} 2>/dev/null")
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
            f"echo '--- log tail ---'; tail -8 {_log_path(role)} 2>/dev/null")
        print(out)
        c.close()


def _smoke_socks(socks_port=DEFAULT_SOCKS_PORT):
    cg = connect("entry")
    kz_ip = _exit_bind_ip() or _role_host("exit")["host"]
    user=os.environ.get("NB_SOCKS_USERNAME");password=os.environ.get("NB_SOCKS_PASSWORD")
    if not user or not password:
        cg.close();raise RuntimeError("SOCKS 冒烟需要 NB_SOCKS_USERNAME 和 NB_SOCKS_PASSWORD")
    auth=shlex.quote(f"{user}:{password}")
    actual=""
    attempts=[]
    max_attempts=max(1,min(60,int(os.environ.get("NB_DEPLOY_SMOKE_ATTEMPTS","24"))))
    for attempt in range(1,max_attempts+1):
        actual=run(cg,f"curl -fsS --proxy-user {auth} --socks5-hostname 127.0.0.1:{socks_port} http://ipinfo.io/ip --max-time 10",tmo=15).strip()
        attempts.append(f"try={attempt} exit={actual or '(empty)'}")
        if actual==kz_ip:break
        time.sleep(3)
    direct=run(cg,"curl -fsS http://ipinfo.io/ip --max-time 10",tmo=15).strip()
    tail=run(cg,f"tail -6 {_log_path('entry')} 2>/dev/null",tmo=15)
    cg.close()
    smoke="\n".join(attempts)+f"\nexpected={kz_ip}\ndirect={direct}\n--- entry log ---\n{tail}"
    print("=== SOCKS5 冒烟(gz entry) ===\n"+smoke)
    if actual!=kz_ip:raise RuntimeError(f"SOCKS 出口验证失败: expected={kz_ip}, actual={actual or '(empty)'}")



__all__ = tuple(name for name in globals() if not name.startswith("__"))
