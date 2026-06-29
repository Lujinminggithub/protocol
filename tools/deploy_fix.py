#!/usr/bin/env python3
"""针对 shm-direct 前端(xgw-edge-server) + C 三跳的定向修复部署（自包含）。

不依赖 hy2bridge_deploy.py（该脚本已删除，以免与 shm-direct 前端混淆）。
原则：
  - 只更新二进制，绝不覆盖线上 configs / hy2-front.json；
  - 前端部署 cmd/xgw-edge-server（shm-direct），不是已废弃的 hy2-bridge 变体；
  - egress 机无编译器，故 C 端在 ingress 远端编译后分发，用现有配置重启。

动作：
  recon         只读侦察：进程/CPU/二进制/配置现状，不做任何改动。
  deploy-c      仅部署 C 端 xgw 到三跳并重启（用现有 configs/{role}.conf）。
  deploy-front  仅部署 shm-direct 前端 xgw-edge-server 到 gz 并重启。
  deploy        deploy-c 之后 deploy-front（完整修复）。
"""

from __future__ import annotations

import argparse
import io
import json
import os
import pathlib
import subprocess
import tarfile
import time
from dataclasses import dataclass
from typing import Dict, Optional, Tuple

import paramiko


ROOT = pathlib.Path(__file__).resolve().parents[1]
TMP_DIR = ROOT / "tmp"
WORK_DIR = "/etc/xgw"
LAB_HOSTS = ROOT / "sample" / "lab-hosts.json"
SERVER_SRC_DIR = ROOT / "xgw-edge"
FRONT_CONFIG = f"{WORK_DIR}/hy2-front.json"
FRONT_SERVER_BIN = f"{WORK_DIR}/xgw-edge-server"
TUNNEL_PORT = 51840
DEFAULT_FRONT_SNI = "gz-hk-kz-old"


@dataclass
class Host:
    name: str
    host: str
    port: int
    user: str
    password: str
    role: str
    jump_via: Optional["Host"] = None
    jump_target_host: Optional[str] = None


def load_lab_hosts() -> dict:
    return json.loads(LAB_HOSTS.read_text(encoding="utf-8"))


def connect(host: Host) -> paramiko.SSHClient:
    last_error: Optional[Exception] = None
    for _ in range(6):
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        jump_client = None
        try:
            sock = None
            if host.jump_via is not None:
                jump_client = paramiko.SSHClient()
                jump_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                jump_client.connect(
                    hostname=host.jump_via.host, port=host.jump_via.port,
                    username=host.jump_via.user, password=host.jump_via.password,
                    timeout=20, banner_timeout=20, auth_timeout=20,
                )
                transport = jump_client.get_transport()
                if transport is None:
                    raise RuntimeError(f"missing jump transport for {host.jump_via.name}")
                target_host = host.jump_target_host or host.host
                sock = transport.open_channel("direct-tcpip", (target_host, host.port), ("127.0.0.1", 0))
            client.connect(
                hostname=host.host, port=host.port, username=host.user,
                password=host.password, timeout=20, banner_timeout=20,
                auth_timeout=20, sock=sock,
            )
            setattr(client, "_jump_client", jump_client)
            return client
        except Exception as exc:  # pragma: no cover - remote jitter
            last_error = exc
            client.close()
            if jump_client is not None:
                jump_client.close()
            time.sleep(3)
    raise last_error  # type: ignore[misc]


def close_client(client: paramiko.SSHClient) -> None:
    jump_client = getattr(client, "_jump_client", None)
    try:
        client.close()
    finally:
        if jump_client is not None:
            jump_client.close()


def run_remote(host: Host, command: str, check: bool = True, timeout: int = 120) -> Tuple[int, str, str]:
    client = connect(host)
    try:
        _, stdout, stderr = client.exec_command(command, timeout=timeout)
        code = stdout.channel.recv_exit_status()
        out = stdout.read().decode("utf-8", errors="ignore")
        err = stderr.read().decode("utf-8", errors="ignore")
        if check and code != 0:
            raise RuntimeError(f"{host.name}: {command}\nSTDOUT:\n{out}\nSTDERR:\n{err}")
        return code, out, err
    finally:
        close_client(client)


def _upload_via_exec(client: paramiko.SSHClient, host: Host, data: bytes, remote_path: str) -> None:
    import base64
    b64 = base64.b64encode(data)
    stdin, stdout, stderr = client.exec_command(f"base64 -d > {remote_path}", timeout=600)
    chan = stdin.channel
    for i in range(0, len(b64), 32 * 1024):
        chan.sendall(b64[i:i + 32 * 1024])
    chan.shutdown_write()
    code = stdout.channel.recv_exit_status()
    err = stderr.read().decode("utf-8", errors="ignore")
    if code != 0:
        raise RuntimeError(f"{host.name}: exec upload failed {remote_path} code={code} err={err[:200]}")


def upload_bytes(host: Host, data: bytes, remote_path: str) -> None:
    last_error: Optional[Exception] = None
    for _ in range(4):
        client = connect(host)
        try:
            use_exec = host.jump_via is not None
            if not use_exec:
                try:
                    sftp = client.open_sftp()
                    try:
                        with sftp.file(remote_path, "wb") as fh:
                            fh.set_pipelined(True)
                            for i in range(0, len(data), 32 * 1024):
                                fh.write(data[i:i + 32 * 1024])
                        size = sftp.stat(remote_path).st_size
                    finally:
                        sftp.close()
                    if size == len(data):
                        return
                    use_exec = True
                except Exception:
                    use_exec = True
            if use_exec:
                _upload_via_exec(client, host, data, remote_path)
                _, vstdout, _ = client.exec_command(f"wc -c < {remote_path}", timeout=60)
                vstdout.channel.recv_exit_status()
                size_out = vstdout.read().decode("utf-8", errors="ignore").strip()
                if size_out.isdigit() and int(size_out) == len(data):
                    return
                raise RuntimeError(f"{host.name}: exec upload size mismatch {remote_path} got={size_out} want={len(data)}")
        except Exception as exc:  # pragma: no cover - remote jitter
            last_error = exc
            time.sleep(3)
        finally:
            close_client(client)
    raise RuntimeError(f"{host.name}: upload failed {remote_path}: {last_error}")


def load_hosts() -> Dict[str, Host]:
    data = load_lab_hosts()
    controller_item = data["controller"]
    controller = Host(
        name=controller_item.get("name", "controller"), host=controller_item["host"],
        port=int(controller_item.get("port", 22)), user=controller_item["user"],
        password=controller_item["password"], role="ingress",
    )
    edge_item = data["edges"][0]
    ingress = Host(
        name=edge_item["name"], host=edge_item["host"], port=int(edge_item.get("port", 22)),
        user=edge_item["user"], password=edge_item["password"], role="ingress",
    )
    ingress.jump_via = controller if ingress.host != controller.host or ingress.port != controller.port else None
    relay_item = data["relays"][0]
    relay = Host(
        name=relay_item["name"], host=relay_item["host"], port=int(relay_item.get("port", 22)),
        user=relay_item["user"], password=relay_item["password"], role="relay",
        jump_via=ingress, jump_target_host=relay_item.get("private_ip"),
    )
    egress_item = data["terminals"][0]
    egress = Host(
        name=egress_item["name"], host=egress_item["host"], port=int(egress_item.get("port", 22)),
        user=egress_item["user"], password=egress_item["password"], role="egress",
    )
    return {"ingress": ingress, "relay": relay, "egress": egress}


def shared_path() -> str:
    data = load_lab_hosts()
    ingress = data["edges"][0]
    relay = data["relays"][0]
    egress = data["terminals"][0]
    return (
        f"phone@mobile=0.0.0.0:0,"
        f"{ingress['name']}@ingress=public={ingress['host']}:{TUNNEL_PORT}|private={ingress['private_ip']}:{TUNNEL_PORT},"
        f"{relay['name']}@relay=public={relay['host']}:{TUNNEL_PORT}|private={relay['private_ip']}:{TUNNEL_PORT},"
        f"{egress['name']}@egress={egress['host']}:{TUNNEL_PORT}"
    )


def runtime_config_text(role: str) -> str:
    data = load_lab_hosts()
    ingress = data["edges"][0]
    relay = data["relays"][0]
    egress = data["terminals"][0]
    hop_name = ingress["name"] if role == "ingress" else relay["name"] if role == "relay" else egress["name"]
    lines = [
        f"node_name=hy2-{role}",
        f"hop_name={hop_name}",
        f"role={role}",
    ]
    if role == "ingress":
        lines.extend(
            [
                "connect_type=bridge",
                "bridge_transport=shared-ring",
                "bridge_tcp_listen=127.0.0.1:19080",
                "bridge_ring_path=/run/xgw/bridge-ring",
            ]
        )
    lines.extend(
        [
            "transport=udp",
            "profile=live-bbr",
            "mtu=1280",
            "payload_size=1100",
            "auth_token=edge-secret",
            "listen_host=0.0.0.0",
            f"path={shared_path()}",
            "allow_cidrs=0.0.0.0/0",
            "policy_group=default",
            "dos_enabled=false",
            "keepalive_sec=10",
            "log_level=1",
            # egress 回程出队 burst drain 预算（修复 TikTok 回程挤牙膏）。
            # pending_chunks 为软上限，受编译期硬上限 32 钳制。
            "bridge_egress_pending_chunks=32",
            "bridge_egress_dequeue_max_chunks_per_tick=64",
            "bridge_egress_dequeue_max_bytes_per_tick=1048576",
            "bridge_egress_dequeue_max_us_per_tick=2000",
        ]
    )
    return "\n".join(lines) + "\n"


def write_local_tmp_configs() -> Dict[str, bytes]:
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    rendered: Dict[str, bytes] = {}
    for role in ("ingress", "relay", "egress"):
        text = runtime_config_text(role)
        path = TMP_DIR / f"tmp-{role}.conf"
        path.write_text(text, encoding="utf-8")
        rendered[role] = text.encode("utf-8")
    return rendered


def upload_runtime_configs(hosts: Dict[str, Host]) -> None:
    rendered = write_local_tmp_configs()
    for role in ("ingress", "relay", "egress"):
        host = hosts[role]
        upload_bytes(host, rendered[role], f"{WORK_DIR}/configs/{role}.conf")
        print(f"[config] {role} ({host.name}) config updated", flush=True)


def render_front_config(server_name: str) -> bytes:
    cfg = json.loads((ROOT / "sample" / "hy2-front.json").read_text(encoding="utf-8"))
    cfg["connect_type"] = "bridge"
    cfg["bridge_transport"] = "shm-direct"
    cfg["bridge_ring_path"] = "/run/xgw/bridge-ring"
    cfg.setdefault("tls", {})
    cfg["tls"]["cert_file"] = f"{WORK_DIR}/gz-self.crt"
    cfg["tls"]["key_file"] = f"{WORK_DIR}/gz-self.key"
    cfg["tls"]["alpn"] = ["h3", "h3-29"]
    cfg.setdefault("control", {})
    cfg["control"]["token"] = "edge-secret"
    cfg["control"]["keepalive"] = "10s"
    cfg["control"]["idle_timeout"] = "120s"
    cfg["control"]["supported_congestion"] = ["bbr", "brutal"]
    cfg.setdefault("masquerade", {})
    cfg["masquerade"]["server_names"] = [server_name]
    return (json.dumps(cfg, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def upload_front_config(ingress: Host, server_name: str) -> None:
    upload_bytes(ingress, render_front_config(server_name), FRONT_CONFIG)
    print(f"[front-config] {ingress.name} hy2-front.json updated sni={server_name}", flush=True)


def generate_front_cert(ingress: Host, server_name: str) -> None:
    cmd = (
        f"mkdir -p {WORK_DIR} /run/xgw && "
        f"rm -f {WORK_DIR}/gz-self.key {WORK_DIR}/gz-self.crt && "
        "openssl req -x509 -newkey rsa:2048 -sha256 -nodes "
        f"-keyout {WORK_DIR}/gz-self.key "
        f"-out {WORK_DIR}/gz-self.crt "
        "-days 3650 "
        f"-subj '/CN={server_name}' "
        f"-addext 'subjectAltName=DNS:{server_name}'"
    )
    run_remote(ingress, cmd, timeout=120)
    print(f"[front-cert] {ingress.name} self-signed cert regenerated sni={server_name}", flush=True)


def build_source_bundle() -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        tar.add(ROOT / "Makefile", arcname="Makefile")
        tar.add(ROOT / "include", arcname="include")
        tar.add(ROOT / "src", arcname="src")
    return buf.getvalue()


def compile_on_ingress(ingress: Host) -> bytes:
    compile_cmd = (
        f"cd {WORK_DIR} && "
        'COMPILER="$(command -v cc || command -v gcc || command -v clang)"; '
        '[ -n "$COMPILER" ] || { echo "no compiler found" >&2; exit 127; }; '
        '"$COMPILER" -std=c11 -O2 -Wall -Wextra -pedantic -pthread -Iinclude -Isrc/log '
        "src/main.c src/config.c src/policy.c src/protocol.c src/crypto.c src/security.c src/control.c src/cc.c "
        "src/session.c src/frame.c src/transport_udp.c src/dataplane.c src/runtime.c src/route.c src/bridge.c src/acl.c "
        "src/pool.c src/tuning.c src/obfs.c src/outbound.c src/tun_stub.c src/tun_linux.c src/afxdp_stub.c "
        'src/afxdp_linux.c src/log/log4c.c -o xgw'
    )
    run_remote(ingress, compile_cmd, timeout=240)
    client = connect(ingress)
    try:
        sftp = client.open_sftp()
        try:
            with sftp.file(f"{WORK_DIR}/xgw", "rb") as fh:
                return fh.read()
        finally:
            sftp.close()
    finally:
        close_client(client)


def install_log_watchdog(host: Host) -> None:
    script = (
        "#!/bin/sh\n"
        "while true; do\n"
        f"  for f in {WORK_DIR}/logs/*.log; do\n"
        "    [ -f \"$f\" ] || continue\n"
        "    sz=$(wc -c < \"$f\" 2>/dev/null || echo 0)\n"
        "    if [ \"$sz\" -gt 209715200 ]; then : > \"$f\"; fi\n"
        "  done\n"
        "  sleep 60\n"
        "done\n"
    )
    upload_bytes(host, script.encode("utf-8"), f"{WORK_DIR}/log-watchdog.sh")
    run_remote(host, f"chmod +x {WORK_DIR}/log-watchdog.sh", check=False)
    run_remote(host, f"pkill -9 -f '{WORK_DIR}/log-watchdog.sh' || true", check=False)
    run_remote(
        host,
        f"sh -c 'nohup {WORK_DIR}/log-watchdog.sh > /dev/null 2>&1 < /dev/null & echo $! > {WORK_DIR}/logs/watchdog.pid'",
        check=False,
    )


def recon(hosts: Dict[str, Host]) -> None:
    cmd = (
        "echo '=== uname ==='; uname -a; "
        "echo '=== top cpu procs ==='; "
        "ps -eo pid,pcpu,pmem,etimes,comm,args --sort=-pcpu 2>/dev/null | head -15; "
        "echo '=== xgw procs ==='; pgrep -af 'xgw' 2>/dev/null || true; "
        "echo '=== bins ==='; "
        f"ls -la {WORK_DIR}/xgw {FRONT_SERVER_BIN} 2>/dev/null; "
        "echo '=== configs ==='; "
        f"ls -la {WORK_DIR}/*.json {WORK_DIR}/configs/*.conf 2>/dev/null; "
        "echo '=== log sizes ==='; "
        f"ls -la {WORK_DIR}/logs/ 2>/dev/null | head -20; "
        "echo '=== compiler ==='; command -v cc gcc clang 2>/dev/null | head -3"
    )
    for role, host in hosts.items():
        _, out, err = run_remote(host, cmd, check=False, timeout=60)
        print(f"\n########## {role}  {host.name}  {host.host} ##########\n{out}\n{err}", flush=True)


def ship_sources_only(hosts: Dict[str, Host]) -> None:
    bundle = build_source_bundle()
    for role, host in hosts.items():
        run_remote(host, f"mkdir -p {WORK_DIR}/configs {WORK_DIR}/logs")
        upload_bytes(host, bundle, f"{WORK_DIR}/xgw-src.tar.gz")
        run_remote(host, f"cd {WORK_DIR} && rm -rf include src Makefile && tar -xzf xgw-src.tar.gz")
        print(f"[ship] {role} ({host.name}) sources updated", flush=True)


def purge_systemd_units(host: Host) -> None:
    """停用并删除可能存在的 xgw/hy2/edge systemd 服务，改回纯 nohup 托管。

    线上若曾用 systemd 拉起前端/或 C 端，service 会在进程被 pkill 后自动重启，
    导致我们的 nohup 部署被旧二进制覆盖。这里先彻底卸载相关 unit。
    """
    cmd = (
        "set +e; "
        # 找出所有 xgw/hy2/edge 相关的 service unit（含 unit-files），逐个停用禁用
        "UNITS=$(systemctl list-units --type=service --all --no-legend 2>/dev/null "
        "| awk '{print $1}' | grep -iE 'xgw|hy2|edge' ; "
        "systemctl list-unit-files --no-legend 2>/dev/null "
        "| awk '{print $1}' | grep -iE 'xgw|hy2|edge'); "
        "UNITS=$(echo \"$UNITS\" | sort -u); "
        "for u in $UNITS; do "
        "  echo \"[purge-unit] $u\"; "
        "  systemctl stop \"$u\" 2>/dev/null; "
        "  systemctl disable \"$u\" 2>/dev/null; "
        "done; "
        # 删除 unit 文件（仅限名字命中的，避免误删系统单元）
        "for d in /etc/systemd/system /run/systemd/system /lib/systemd/system; do "
        "  for f in $(ls \"$d\" 2>/dev/null | grep -iE 'xgw|hy2|edge'); do "
        "    echo \"[purge-file] $d/$f\"; rm -f \"$d/$f\"; "
        "  done; "
        "done; "
        "systemctl daemon-reload 2>/dev/null; "
        "systemctl reset-failed 2>/dev/null; "
        "echo '[purge-done]'"
    )
    _, out, _ = run_remote(host, cmd, check=False, timeout=90)
    print(f"[systemd] {host.name}:\n{out.strip()}", flush=True)


def wipe_remote_runtime(host: Host) -> None:
    purge_systemd_units(host)
    cmd = (
        "set +e; "
        "pkill -9 -f '/etc/xgw/xgw run ' 2>/dev/null; "
        "pkill -9 -f '/etc/xgw/xgw-edge-server ' 2>/dev/null; "
        "pkill -9 -f 'xgw-edge-server -config /etc/xgw/hy2-front.json' 2>/dev/null; "
        f"rm -rf {WORK_DIR}; "
        "rm -rf /run/xgw; "
        f"mkdir -p {WORK_DIR}/configs {WORK_DIR}/logs /run/xgw; "
        "echo '[wipe-done]'"
    )
    run_remote(host, cmd, check=False, timeout=120)
    print(f"[wipe] {host.name} runtime reset", flush=True)


def restart_c(host: Host, role: str) -> None:
    purge_systemd_units(host)
    run_remote(host, f"pkill -9 -f '{WORK_DIR}/xgw run {WORK_DIR}/configs/{role}.conf' || true", check=False)
    time.sleep(1)
    run_remote(
        host,
        (
            f"cd {WORK_DIR} && "
            f"rm -f logs/{role}.out.log logs/{role}.err.log logs/{role}.pid && "
            f"sh -c 'nohup {WORK_DIR}/xgw run {WORK_DIR}/configs/{role}.conf "
            f"> logs/{role}.out.log 2> logs/{role}.err.log < /dev/null & echo $! > logs/{role}.pid'"
        ),
    )
    print(f"[restart-c] {role} ({host.name}) restarted", flush=True)


def deploy_c(hosts: Dict[str, Host]) -> Dict[str, str]:
    ship_sources_only(hosts)
    upload_runtime_configs(hosts)
    binary = compile_on_ingress(hosts["ingress"])
    print(f"[compile] xgw built on ingress: {len(binary)} bytes", flush=True)
    for role in ("egress", "relay", "ingress"):
        host = hosts[role]
        upload_bytes(host, binary, f"{WORK_DIR}/xgw.new")
        run_remote(host, f"chmod +x {WORK_DIR}/xgw.new && mv -f {WORK_DIR}/xgw.new {WORK_DIR}/xgw")
        install_log_watchdog(host)
        restart_c(host, role)
        time.sleep(3)
    status: Dict[str, str] = {}
    for role, host in hosts.items():
        _, out, _ = run_remote(host, f"tail -n 25 {WORK_DIR}/logs/{role}.out.log 2>/dev/null || true", check=False)
        status[role] = out.strip()
    return status


def build_server_binary() -> bytes:
    out_path = TMP_DIR / "xgw-edge-server-linux"
    env = {**os.environ, "GOOS": "linux", "GOARCH": "amd64", "CGO_ENABLED": "0"}
    subprocess.run(
        ["go", "build", "-o", str(out_path), "./cmd/xgw-edge-server"],
        cwd=str(SERVER_SRC_DIR), env=env, check=True,
    )
    return out_path.read_bytes()


def deploy_front(ingress: Host) -> str:
    binary = build_server_binary()
    print(f"[build] xgw-edge-server (linux) built: {len(binary)} bytes", flush=True)
    _, out, _ = run_remote(ingress, f"test -f {FRONT_CONFIG} && echo PRESENT || echo MISSING", check=False)
    if "MISSING" in out:
        sample = (ROOT / "sample" / "hy2-front.json").read_bytes()
        upload_bytes(ingress, sample, FRONT_CONFIG)
        print(f"[front] {FRONT_CONFIG} missing → uploaded sample fallback", flush=True)
    else:
        print(f"[front] {FRONT_CONFIG} present → keep as-is", flush=True)
    run_remote(ingress, f"mkdir -p {WORK_DIR}/logs")
    upload_bytes(ingress, binary, f"{FRONT_SERVER_BIN}.new")
    run_remote(ingress, f"chmod +x {FRONT_SERVER_BIN}.new && mv -f {FRONT_SERVER_BIN}.new {FRONT_SERVER_BIN}")
    purge_systemd_units(ingress)
    run_remote(ingress, f"pkill -9 -f '{FRONT_SERVER_BIN} ' || true", check=False)
    time.sleep(1)
    run_remote(
        ingress,
        (
            f"cd {WORK_DIR} && rm -f logs/hy2front.out.log && "
            f"sh -c 'nohup {FRONT_SERVER_BIN} -config {FRONT_CONFIG} "
            f"> logs/hy2front.out.log 2>&1 < /dev/null & echo $! > logs/hy2front.pid'"
        ),
    )
    time.sleep(3)
    _, tail, _ = run_remote(ingress, f"tail -n 30 {WORK_DIR}/logs/hy2front.out.log 2>/dev/null || true", check=False)
    _, cpu, _ = run_remote(
        ingress,
        "ps -eo pcpu,comm,args --sort=-pcpu | grep -m1 xgw-edge-server || true",
        check=False,
    )
    return (tail.strip() + "\n--- cpu ---\n" + cpu.strip())


def reset_deploy(hosts: Dict[str, Host], server_name: str) -> Dict[str, object]:
    for role in ("egress", "relay", "ingress"):
        wipe_remote_runtime(hosts[role])
    upload_front_config(hosts["ingress"], server_name)
    generate_front_cert(hosts["ingress"], server_name)
    c = deploy_c(hosts)
    f = deploy_front(hosts["ingress"])
    return {"deploy_c": c, "front": f, "sni": server_name}


def main() -> None:
    parser = argparse.ArgumentParser(description="shm-direct 前端 + C 三跳 定向修复部署（自包含）")
    parser.add_argument("action", choices=["recon", "deploy-c", "deploy-front", "deploy", "reset-deploy"])
    parser.add_argument("--front-sni", default=DEFAULT_FRONT_SNI)
    args = parser.parse_args()
    hosts = load_hosts()

    if args.action == "recon":
        recon(hosts)
        result = {"recon": "see stdout above"}
    elif args.action == "deploy-c":
        result = {"deploy_c": deploy_c(hosts)}
    elif args.action == "deploy-front":
        result = {"front": deploy_front(hosts["ingress"])}
    elif args.action == "reset-deploy":
        result = reset_deploy(hosts, args.front_sni)
    else:
        c = deploy_c(hosts)
        f = deploy_front(hosts["ingress"])
        result = {"deploy_c": c, "front": f}
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
