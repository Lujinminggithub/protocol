#!/usr/bin/env python3
import argparse
import io
import json
import logging
import pathlib
import shlex
import tarfile
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

import paramiko

logging.getLogger("paramiko").setLevel(logging.CRITICAL)

ROOT = pathlib.Path(__file__).resolve().parents[1]
SAMPLE_DIR = ROOT / "sample"
TMP_DIR = ROOT / "tmp" / "smoke"
GENERATED_DIR = TMP_DIR / "generated"
BUNDLE_PATH = TMP_DIR / "xgw-smoke-src.tar.gz"
BUILT_BINARY_PATH = TMP_DIR / "xgw-linux-amd64"
SUMMARY_DIR = TMP_DIR / "summaries"


@dataclass
class Host:
    name: str
    host: str
    port: int
    user: str
    password: str
    role: str
    private_ip: Optional[str] = None
    jump_via: Optional[str] = None

    @property
    def route_ip(self) -> str:
        return self.private_ip or self.host


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text(encoding="utf-8"))


def build_host_index(data: Dict) -> Dict[str, Host]:
    index: Dict[str, Host] = {}
    groups = [data.get("edges", []), data.get("relays", []), data.get("terminals", [])]
    for group in groups:
        for item in group:
            index[item["name"]] = Host(
                name=item["name"],
                host=item["host"],
                port=int(item.get("port", 22)),
                user=item["user"],
                password=item["password"],
                role=item["role"],
                private_ip=item.get("private_ip"),
                jump_via=item.get("jump_via"),
            )
    return index


def render_path(topology: Dict, role: Optional[str] = None) -> str:
    runtime_port = int(topology["runtime"]["tunnel_port"])
    ingress = topology["ingress"]
    relay = topology["relay"]
    egress = topology["egress"]
    ingress_public = ingress["public_host"]
    ingress_private = ingress.get("private_host")
    relay_public = relay["public_host"]
    relay_private = relay.get("private_host")
    ingress_addr = f"{ingress_public}:{runtime_port}"
    relay_addr = f"{relay_public}:{runtime_port}"
    if ingress_private:
        ingress_addr = f"public={ingress_public}:{runtime_port}|private={ingress_private}:{runtime_port}"
    if relay_private:
        relay_addr = f"public={relay_public}:{runtime_port}|private={relay_private}:{runtime_port}"
    return (
        f"phone@mobile=0.0.0.0:0,"
        f"{ingress['name']}@ingress={ingress_addr},"
        f"{relay['name']}@relay={relay_addr},"
        f"{egress['name']}@egress={egress['public_host']}:{runtime_port}"
    )


def set_runtime_port(topology: Dict, port: int) -> Dict:
    topo = json.loads(json.dumps(topology))
    topo["runtime"]["tunnel_port"] = int(port)
    return topo


def set_line_identity(topology: Dict, line_id: str) -> Dict:
    topo = json.loads(json.dumps(topology))
    topo["path_name"] = line_id
    topo.setdefault("runtime", {})["line_id"] = line_id
    return topo


def whitelist_ips(topology: Dict) -> str:
    values = [
        topology["ingress"]["public_host"],
        topology["relay"]["public_host"],
        topology["egress"]["public_host"],
    ]
    return ",".join(values)


def render_config(topology: Dict, role: str, profile: str = "live-bbr") -> str:
    runtime = topology["runtime"]
    path = render_path(topology, role=role)
    whitelist = whitelist_ips(topology)
    runtime_port = int(runtime["tunnel_port"])
    if profile == "live-brutal":
        fec_parity_shards = "2"
        pacing_rate_bps = "200000000"
        pacing_interval_us = "50"
        redundant_copies = "2"
        mtu_profile = "mobile-safe"
        payload_profile = "safe"
    else:
        fec_parity_shards = "1"
        pacing_rate_bps = "50000000"
        pacing_interval_us = "200"
        redundant_copies = "1"
        mtu_profile = "relay-balanced"
        payload_profile = "balanced"
    base = {
        "ingress": {
            "node_name": f"smoke-{topology['ingress']['name']}",
            "hop_name": topology["ingress"]["name"],
            "listen_host": "0.0.0.0",
            "tun_name": f"{runtime.get('tun_name', 'xgw')}-ing",
            "tun_addr": "10.23.0.1/24",
            "device": "eth0",
            "queue_id": "0",
            "role": "ingress",
        },
        "relay": {
            "node_name": f"smoke-{topology['relay']['name']}",
            "hop_name": topology["relay"]["name"],
            "listen_host": "0.0.0.0",
            "device": "eth0",
            "queue_id": "0",
            "role": "relay",
        },
        "egress": {
            "node_name": f"smoke-{topology['egress']['name']}",
            "hop_name": topology["egress"]["name"],
            "listen_host": "0.0.0.0",
            "tun_name": f"{runtime.get('tun_name', 'xgw')}-eg",
            "tun_addr": "10.23.0.3/24",
            "device": "eth0",
            "queue_id": "0",
            "role": "egress",
        },
    }[role]

    lines = [
        f"node_name={base['node_name']}",
        f"transport={runtime.get('transport', 'udp')}",
        f"proxy_mode={runtime.get('proxy_mode', 'fixed-path')}",
        "acl_mode=allow",
        f"mtu_profile={mtu_profile}",
        f"payload_profile={payload_profile}",
        f"hop_name={base['hop_name']}",
        f"listen_host={base['listen_host']}",
    ]
    if "tun_name" in base:
        lines.append(f"tun_name={base['tun_name']}")
    if "tun_addr" in base:
        lines.append(f"tun_addr={base['tun_addr']}")
    lines.extend(
        [
            f"device={base['device']}",
            f"queue_id={base['queue_id']}",
            f"role={base['role']}",
            f"profile={profile}",
            f"congestion={profile.replace('live-', '')}",
            "bbr_profile=standard",
            "auth_token=smoke-auth-token",
            "advertised_rx_bps=0",
            "advertised_tx_bps=0",
            "initial_stream_receive_window=8388608",
            "max_stream_receive_window=8388608",
            "initial_connection_receive_window=20971520",
            "max_connection_receive_window=20971520",
            "max_idle_timeout_sec=30",
            "keepalive_sec=10",
            "disable_path_mtu_discovery=false",
            "enable_udp=true",
            f"path={path}",
            "",
            f"mtu={int(runtime.get('mtu', 1380))}",
            "payload_size=1200",
            "reorder_window=128",
            "fec_data_shards=4",
            f"fec_parity_shards={fec_parity_shards}",
            f"pacing_rate_bps={pacing_rate_bps}",
            f"pacing_interval_us={pacing_interval_us}",
            f"redundant_copies={redundant_copies}",
            "",
            "policy_group=default",
            "allow_cidrs=0.0.0.0/0",
            "allow_domains=live.tiktok.com",
            "allow_domain_suffixes=tiktokcdn.com,byteoversea.com",
            "conservative_domain_allow=true",
            "dynamic_grace_period_sec=30",
            "",
            "dos_enabled=true",
            "max_connections=1000",
            "max_connections_per_ip=200",
            "rate_limit_per_second=2000",
            "rate_limit_burst=2000",
            "blacklist_duration_sec=300",
            f"whitelist_ips={whitelist}",
            f"outbound_type={runtime.get('outbound_type', 'direct')}",
            "outbound_host=",
            "outbound_port=0",
            "outbound_username=",
            "outbound_password=",
            "obfs_mode=salamander",
            "obfs_scope=hop",
            "obfs_key=smoke-secret-key",
            "udp_rcvbuf_bytes=4194304",
            "udp_sndbuf_bytes=4194304",
            "log_level=2",
            "enable_debug_timing=true",
            "enable_summary_dump=true",
            "",
            f"# tunnel_port={runtime_port}",
        ]
    )
    return "\n".join(lines) + "\n"


def generate_configs(topology_path: pathlib.Path, output_dir: pathlib.Path, write_sample: bool = False, profile: str = "live-bbr") -> Dict[str, pathlib.Path]:
    topology = load_json(topology_path)
    return generate_configs_from_topology(topology, output_dir, write_sample=write_sample, profile=profile)


def generate_configs_from_topology(topology: Dict, output_dir: pathlib.Path, write_sample: bool = False, profile: str = "live-bbr") -> Dict[str, pathlib.Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    rendered: Dict[str, pathlib.Path] = {}
    for role in ("ingress", "relay", "egress"):
        path = output_dir / f"smoke-{role}.conf"
        path.write_text(render_config(topology, role, profile=profile), encoding="utf-8")
        rendered[role] = path
        if write_sample:
            sample_path = SAMPLE_DIR / f"smoke-{role}.conf"
            sample_path.write_text(path.read_text(encoding="utf-8"), encoding="utf-8")
    return rendered


def render_single_multiline_config(topologies: List[Tuple[str, Dict]], role: str, profile: str = "live-bbr") -> str:
    if not topologies:
        raise RuntimeError("single multiline config requires at least one topology")
    base = render_config(topologies[0][1], role, profile=profile).splitlines()
    filtered: List[str] = []
    for line in base:
        if line.startswith("path="):
            continue
        filtered.append(line)
        if line.startswith("pool_select="):
            filtered.append("default_line=primary")
            filtered.append("route_control_path=logs/route-control")
            filtered.append("line_drain_timeout_sec=3")
    if not any(item.startswith("default_line=") for item in filtered):
        filtered.append("default_line=primary")
        filtered.append("route_control_path=logs/route-control")
        filtered.append("line_drain_timeout_sec=3")
    for line_id, topo in topologies:
        filtered.append(f"line.{line_id}=path={render_path(topo, role=role)};priority={100 if line_id == 'primary' else 80};enabled=true")
    return "\n".join(filtered) + "\n"


def generate_single_multiline_configs(topologies: List[Tuple[str, Dict]], output_dir: pathlib.Path, profile: str) -> Dict[str, pathlib.Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    rendered: Dict[str, pathlib.Path] = {}
    for role in ("ingress", "relay", "egress"):
        path = output_dir / f"single-multiline-{role}.conf"
        path.write_text(render_single_multiline_config(topologies, role, profile=profile), encoding="utf-8")
        rendered[role] = path
    return rendered


def write_resolved_topology(topology: Dict, output_dir: pathlib.Path) -> pathlib.Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    resolved_topology_path = output_dir / "resolved-topology.json"
    resolved_topology_path.write_text(json.dumps(topology, indent=2), encoding="utf-8")
    return resolved_topology_path


def build_bundle() -> pathlib.Path:
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    with tarfile.open(BUNDLE_PATH, "w:gz") as tar:
        tar.add(ROOT / "Makefile", arcname="Makefile")
        tar.add(ROOT / "include", arcname="include")
        tar.add(ROOT / "src", arcname="src")
        tar.add(ROOT / "bpf", arcname="bpf")
        tar.add(ROOT / "tools" / "smoke_tcp_udp_echo.py", arcname="tools/smoke_tcp_udp_echo.py")
        tar.add(ROOT / "tools" / "smoke_http_connect_proxy.py", arcname="tools/smoke_http_connect_proxy.py")
        tar.add(ROOT / "tools" / "smoke_socks5_proxy.py", arcname="tools/smoke_socks5_proxy.py")
    return BUNDLE_PATH


def wait_remote_log(host: Host,
                    host_index: Dict[str, Host],
                    logfile: str,
                    success_markers: List[str],
                    error_markers: List[str],
                    timeout_sec: int = 30) -> Tuple[bool, str]:
    end_at = time.time() + timeout_sec
    while time.time() < end_at:
        _, out, _ = run_remote(host, host_index, f"cat {shlex.quote(logfile)} 2>/dev/null || true", check=False)
        for marker in success_markers:
            if marker in out:
                return True, out
        for marker in error_markers:
            if marker in out:
                return False, out
        time.sleep(2)
    _, out, _ = run_remote(host, host_index, f"cat {shlex.quote(logfile)} 2>/dev/null || true", check=False)
    return False, out


def wait_remote_task(host: Host,
                     host_index: Dict[str, Host],
                     work_dir: str,
                     logfile: str,
                     pidfile: str,
                     success_markers: List[str],
                     error_markers: List[str],
                     timeout_sec: int = 40) -> Dict[str, str]:
    end_at = time.time() + timeout_sec
    last_log = ""
    while time.time() < end_at:
        _, out, _ = run_remote(
            host,
            host_index,
            (
                f"cd {shlex.quote(work_dir)} && "
                f"PID=$(cat {shlex.quote(pidfile)} 2>/dev/null || true); "
                f"LOG=$(cat {shlex.quote(logfile)} 2>/dev/null || true); "
                f"echo '---PID---'; echo \"$PID\"; "
                f"echo '---ALIVE---'; [ -n \"$PID\" ] && ps -p $PID -o pid=,stat=,cmd= || true; "
                f"echo '---LOG---'; echo \"$LOG\""
            ),
            check=False,
        )
        last_log = out
        if "bootstrap" in out and not any(marker in out for marker in success_markers + error_markers):
            time.sleep(2)
            continue
        for marker in success_markers:
            if marker in out:
                return {"ok": "true", "log": out}
        for marker in error_markers:
            if marker in out:
                return {"ok": "false", "log": out}
        time.sleep(2)
    return {"ok": "false", "log": last_log}


def wait_remote_port(host: Host, host_index: Dict[str, Host], port: int, proto: str, timeout_sec: int = 20) -> bool:
    end_at = time.time() + timeout_sec
    pattern = f":{port}"
    flag = "-lnup" if proto == "udp" else "-lntp"
    while time.time() < end_at:
        _, out, _ = run_remote(host, host_index, f"ss {flag} | grep '{pattern}' || true", check=False)
        if pattern in out:
            return True
        time.sleep(1)
    return False


def wait_remote_process(host: Host, host_index: Dict[str, Host], pattern: str, timeout_sec: int = 20) -> bool:
    end_at = time.time() + timeout_sec
    while time.time() < end_at:
        _, out, _ = run_remote(host, host_index, f"pgrep -af {shlex.quote(pattern)} || true", check=False)
        if out.strip():
            return True
        time.sleep(1)
    return False


def wait_remote_tcp_connect(host: Host, host_index: Dict[str, Host], target_host: str, port: int, timeout_sec: int = 20) -> bool:
    end_at = time.time() + timeout_sec
    while time.time() < end_at:
        _, out, _ = run_remote(
            host,
            host_index,
            f"timeout 5s bash -lc 'cat < /dev/null > /dev/tcp/{target_host}/{port}' && echo OK || echo FAIL",
            check=False,
        )
        if "OK" in out:
            return True
        time.sleep(1)
    return False


def extract_logged_port(log_text: str, marker: str) -> int:
    for line in log_text.splitlines():
        if marker in line:
            try:
                return int(line.split(marker, 1)[1].strip())
            except ValueError:
                continue
    return 0


def wait_remote_logged_port(host: Host,
                            host_index: Dict[str, Host],
                            logfile: str,
                            marker: str,
                            timeout_sec: int = 20) -> int:
    end_at = time.time() + timeout_sec
    while time.time() < end_at:
        _, out, _ = run_remote(host, host_index, f"cat {shlex.quote(logfile)} 2>/dev/null || true", check=False)
        port = extract_logged_port(out, marker)
        if port > 0:
            return port
        time.sleep(1)
    _, out, _ = run_remote(host, host_index, f"cat {shlex.quote(logfile)} 2>/dev/null || true", check=False)
    return extract_logged_port(out, marker)


def start_remote_background(host: Host,
                            host_index: Dict[str, Host],
                            work_dir: str,
                            command: str,
                            logfile: str,
                            pidfile: str) -> None:
    run_remote(
        host,
        host_index,
        (
            f"cd {shlex.quote(work_dir)} && mkdir -p logs && "
            f"rm -f {shlex.quote(logfile)} {shlex.quote(pidfile)} && "
            f"sh -c 'echo bootstrap > {shlex.quote(logfile)}; "
            f"nohup {command} >> {shlex.quote(logfile)} 2>&1 < /dev/null & echo $! > {shlex.quote(pidfile)}'"
        ),
        check=False,
    )


def inspect_remote_background(host: Host,
                              host_index: Dict[str, Host],
                              work_dir: str,
                              logfile: str,
                              pidfile: str) -> Dict[str, str]:
    _, out, _ = run_remote(
        host,
        host_index,
        (
            f"cd {shlex.quote(work_dir)} && "
            f"echo '--- pid ---'; cat {shlex.quote(pidfile)} 2>/dev/null || true; "
            f"PID=$(cat {shlex.quote(pidfile)} 2>/dev/null || true); "
            f"echo '--- ps ---'; [ -n \"$PID\" ] && ps -p $PID -o pid,ppid,stat,cmd || true; "
            f"echo '--- ss ---'; ss -lntup || true; "
            f"echo '--- log ---'; cat {shlex.quote(logfile)} 2>/dev/null || true"
        ),
        check=False,
    )
    return {"inspection": out.strip()}


def write_summary(topology: Dict, output_dir: pathlib.Path, profile: str, result: Dict) -> pathlib.Path:
    SUMMARY_DIR.mkdir(parents=True, exist_ok=True)
    summary = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime()),
        "profile": profile,
        "topology": topology,
        "result": result,
    }
    profile_name = profile.replace("/", "_")
    summary_path = SUMMARY_DIR / f"summary-{profile_name}.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    latest_path = output_dir / f"summary-{profile_name}.json"
    latest_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary_path


def write_multiline_summary(output_dir: pathlib.Path, profile: str, summary: Dict) -> pathlib.Path:
    SUMMARY_DIR.mkdir(parents=True, exist_ok=True)
    profile_name = profile.replace("/", "_")
    summary_path = SUMMARY_DIR / f"summary-multiline-{profile_name}.json"
    latest_path = output_dir / f"summary-multiline-{profile_name}.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    latest_path.parent.mkdir(parents=True, exist_ok=True)
    latest_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary_path


def compact_smoke_status(line_id: str, topology: Dict, result: Dict) -> Dict[str, object]:
    probe_result = result.get("probe_result", {})
    evidence = probe_result.get("evidence", [])
    if not isinstance(evidence, list):
        evidence = []
    ingress_ok = any(str(item).startswith("ingress:") for item in evidence)
    relay_ok = any(str(item).startswith("relay:") for item in evidence)
    egress_ok = any(str(item).startswith("egress:") for item in evidence)
    return {
        "line_id": line_id,
        "port": int(topology["runtime"]["tunnel_port"]),
        "ok": bool(probe_result.get("ok")) and ingress_ok and relay_ok and egress_ok and "error" not in result,
        "ingress": ingress_ok,
        "relay": relay_ok,
        "egress": egress_ok,
        "evidence": evidence,
        "failed_stage": result.get("failed_stage", ""),
        "error": result.get("error", ""),
    }


def connect_ssh(host: Host, host_index: Dict[str, Host]) -> paramiko.SSHClient:
    last_error = None
    for _ in range(5):
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        jump_client = None
        try:
            sock = None
            if host.jump_via:
                jump = host_index[host.jump_via]
                jump_client = paramiko.SSHClient()
                jump_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                jump_client.connect(
                    hostname=jump.host,
                    port=jump.port,
                    username=jump.user,
                    password=jump.password,
                    timeout=30,
                    banner_timeout=30,
                    auth_timeout=30,
                )
                transport = jump_client.get_transport()
                if transport is None:
                    raise RuntimeError(f"missing jump transport for {jump.name}")
                sock = transport.open_channel("direct-tcpip", (host.host, host.port), ("127.0.0.1", 0))
            client.connect(
                hostname=host.host,
                port=host.port,
                username=host.user,
                password=host.password,
                timeout=30,
                banner_timeout=30,
                auth_timeout=30,
                sock=sock,
            )
            setattr(client, "_jump_client", jump_client)
            return client
        except Exception as exc:
            last_error = exc
            client.close()
            if jump_client is not None:
                jump_client.close()
            time.sleep(4)
    raise last_error


def close_ssh(client: paramiko.SSHClient) -> None:
    jump_client = getattr(client, "_jump_client", None)
    try:
        client.close()
    finally:
        if jump_client is not None:
            jump_client.close()


def run_remote(host: Host, host_index: Dict[str, Host], command: str, check: bool = True) -> Tuple[int, str, str]:
    client = connect_ssh(host, host_index)
    try:
        _, stdout, stderr = client.exec_command(command)
        code = stdout.channel.recv_exit_status()
        out = stdout.read().decode("utf-8", errors="ignore")
        err = stderr.read().decode("utf-8", errors="ignore")
        if check and code != 0:
            raise RuntimeError(f"{host.name}: {command}\nSTDOUT:\n{out}\nSTDERR:\n{err}")
        return code, out, err
    finally:
        close_ssh(client)


def upload_file(host: Host, host_index: Dict[str, Host], local_path: pathlib.Path, remote_path: str) -> None:
    client = connect_ssh(host, host_index)
    try:
        directory = str(pathlib.PurePosixPath(remote_path).parent)
        client.exec_command(f"mkdir -p {shlex.quote(directory)}")[1].channel.recv_exit_status()
        tmp_remote = f"{remote_path}.uploading"
        stdin, stdout, stderr = client.exec_command(f"cat > {shlex.quote(tmp_remote)}")
        with local_path.open("rb") as fh:
            while True:
                chunk = fh.read(256 * 1024)
                if not chunk:
                    break
                stdin.channel.sendall(chunk)
        stdin.channel.shutdown_write()
        code = stdout.channel.recv_exit_status()
        out = stdout.read().decode("utf-8", errors="ignore")
        err = stderr.read().decode("utf-8", errors="ignore")
        if code != 0:
            raise RuntimeError(f"{host.name}: upload failed for {remote_path}\nSTDOUT:\n{out}\nSTDERR:\n{err}")
        client.exec_command(f"mv -f {shlex.quote(tmp_remote)} {shlex.quote(remote_path)}")[1].channel.recv_exit_status()
    finally:
        close_ssh(client)


def download_file(host: Host, host_index: Dict[str, Host], remote_path: str, local_path: pathlib.Path) -> None:
    client = connect_ssh(host, host_index)
    try:
        local_path.parent.mkdir(parents=True, exist_ok=True)
        stdin, stdout, stderr = client.exec_command(f"cat {shlex.quote(remote_path)}")
        data = stdout.read()
        err = stderr.read().decode("utf-8", errors="ignore")
        code = stdout.channel.recv_exit_status()
        if code != 0:
            raise RuntimeError(f"{host.name}: download failed for {remote_path}\nSTDERR:\n{err}")
        local_path.write_bytes(data)
    finally:
        close_ssh(client)


def remote_role_host(topology: Dict, hosts: Dict[str, Host], role: str) -> Host:
    name = topology[role]["name"]
    return hosts[name]


def remote_work_dir(lab_hosts: Dict) -> str:
    return lab_hosts.get("paths", {}).get("work_dir", "/etc/xgw")


def is_port_busy(host: Host, host_index: Dict[str, Host], port: int) -> bool:
    cmd = f"ss -lun | grep -q ':{port} ' && echo busy || echo free"
    _, out, _ = run_remote(host, host_index, cmd, check=False)
    return out.strip() == "busy"


def choose_free_port(host: Host, host_index: Dict[str, Host], proto: str, candidates: List[int]) -> int:
    flag = "-lnt" if proto == "tcp" else "-lnu"
    for port in candidates:
        _, out, _ = run_remote(host, host_index, f"ss {flag} | grep -q ':{port} ' && echo busy || echo free", check=False)
        if out.strip() == "free":
            return port
    raise RuntimeError(f"{host.name}: no free {proto} port from candidates {candidates}")


def ensure_relay_port(topology: Dict, host_index: Dict[str, Host]) -> Dict:
    relay_host = remote_role_host(topology, host_index, "relay")
    requested = int(topology["runtime"]["tunnel_port"])
    candidates = [requested, requested + 1, requested + 2, 52830, 53830]
    run_remote(relay_host, host_index, f"pkill -f '/etc/xgw/xgw run configs/relay.conf' || true", check=False)
    for candidate in candidates:
        if not is_port_busy(relay_host, host_index, candidate):
            if candidate != requested:
                print(f"[relay-port] switch {requested} -> {candidate} because relay port is busy")
            return set_runtime_port(topology, candidate)
    return set_runtime_port(topology, requested)


def resolve_topology(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path, dynamic_port_check: bool = True) -> Tuple[Dict, Dict, Dict[str, Host]]:
    topology = load_json(topology_path)
    lab_hosts = load_json(lab_hosts_path)
    host_index = build_host_index(lab_hosts)
    if topology_path.name == "resolved-topology.json":
        dynamic_port_check = False
    if dynamic_port_check:
        topology = ensure_relay_port(topology, host_index)
    return topology, lab_hosts, host_index


def cleanup(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path) -> None:
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path, dynamic_port_check=False)
    work_dir = remote_work_dir(lab_hosts)
    port = int(topology["runtime"]["tunnel_port"])
    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        tun_names = ["xgw0", "xgw-ing", "xgw-eg", "xgw0-ing", "xgw0-eg", "xgwuk0"]
        commands = [
            (
                f"if [ -f {shlex.quote(work_dir + '/logs/' + role + '.pid')} ]; then "
                f"kill -9 $(cat {shlex.quote(work_dir + '/logs/' + role + '.pid')}) 2>/dev/null || true; "
                f"rm -f {shlex.quote(work_dir + '/logs/' + role + '.pid')}; fi"
            ),
            f"pkill -9 -f '^./xgw run configs/{role}.conf$' || true",
            f"pkill -9 -f {shlex.quote(work_dir + '/xgw run configs/' + role + '.conf')} || true",
            f"pkill -9 -f {shlex.quote(work_dir + '/xgw run')} || true",
            f"pkill -9 -f {shlex.quote('./xgw run')} || true",
            (
                f"for pid in $(ss -lunp 2>/dev/null | grep ':{port} ' | sed -n 's/.*pid=\\([0-9]\\+\\).*/\\1/p'); do "
                f"kill -9 $pid 2>/dev/null || true; done"
            ),
            f"rm -f {shlex.quote(work_dir + '/logs/' + role + '.log')} 2>/dev/null || true",
        ]
        for tun_name in tun_names:
            commands.append(f"ip link del {shlex.quote(tun_name)} 2>/dev/null || true")
        commands.append("for dev in $(ip -o link show | awk -F': ' '{print $2}' | grep '^xgw'); do ip link del \"$dev\" 2>/dev/null || true; done")
        commands.append(f"rm -f {shlex.quote(work_dir + '/logs/probe.tcpdump')} {shlex.quote(work_dir + '/logs/probe.ss.before')} {shlex.quote(work_dir + '/logs/probe.ss.after')} {shlex.quote(work_dir + '/logs/route-control')} 2>/dev/null || true")
        run_remote(host, host_index, " && ".join(commands), check=False)


def preflight(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path) -> Dict[str, Dict[str, str]]:
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path, dynamic_port_check=False)
    work_dir = remote_work_dir(lab_hosts)
    results: Dict[str, Dict[str, str]] = {}
    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        cmd = (
            "set -e; "
            "echo 'whoami='$(whoami); "
            "echo 'id='$(id -u); "
            "echo 'tun_dev='$(ls -l /dev/net/tun 2>/dev/null || echo missing); "
            "echo 'ip='$(command -v ip || echo missing); "
            "echo 'ss='$(command -v ss || echo missing); "
            "echo 'compiler='$(command -v cc || command -v gcc || command -v clang || echo missing); "
            f"echo 'work_dir='$(test -d {shlex.quote(work_dir)} && echo exists || echo missing); "
            "ip tuntap show 2>/dev/null || true; "
            "ip -o link show | grep 'xgw' || true"
        )
        _, out, err = run_remote(host, host_index, cmd, check=False)
        results[role] = {"stdout": out.strip(), "stderr": err.strip()}
    return results


def compile_command() -> str:
    return (
        'COMPILER="$(command -v cc || command -v gcc || command -v clang)"; '
        '[ -n "$COMPILER" ] || { echo "no compiler found" >&2; exit 127; }; '
        '"$COMPILER" -std=c11 -O2 -Wall -Wextra -pedantic -Iinclude '
        "src/main.c src/config.c src/policy.c src/protocol.c src/crypto.c src/security.c src/control.c src/cc.c src/session.c src/frame.c "
        "src/transport_udp.c src/dataplane.c src/runtime.c src/route.c src/bridge.c src/acl.c src/pool.c src/tuning.c src/obfs.c src/outbound.c src/tun_stub.c src/tun_linux.c "
        "src/afxdp_stub.c src/afxdp_linux.c -o xgw"
    )


def deploy(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path, generated_dir: pathlib.Path, profile: str) -> None:
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path)
    work_dir = remote_work_dir(lab_hosts)
    bundle = build_bundle()
    configs = generate_configs_from_topology(topology, generated_dir, write_sample=False, profile=profile)
    builder_host = remote_role_host(topology, host_index, "ingress")

    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        run_remote(host, host_index, f"mkdir -p {shlex.quote(work_dir)} {shlex.quote(work_dir + '/logs')} {shlex.quote(work_dir + '/configs')}")
        upload_file(host, host_index, bundle, f"{work_dir}/xgw-smoke-src.tar.gz")
        upload_file(host, host_index, configs[role], f"{work_dir}/configs/{role}.conf")
        run_remote(host, host_index, f"cd {shlex.quote(work_dir)} && rm -rf src include bpf Makefile xgw && tar -xzf xgw-smoke-src.tar.gz")

    run_remote(
        builder_host,
        host_index,
        f"cd {shlex.quote(work_dir)} && {compile_command()}",
    )
    download_file(builder_host, host_index, f"{work_dir}/xgw", BUILT_BINARY_PATH)

    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        upload_file(host, host_index, BUILT_BINARY_PATH, f"{work_dir}/xgw")
        run_remote(host, host_index, f"chmod +x {shlex.quote(work_dir + '/xgw')}")


def deploy_with_configs(topology: Dict,
                        lab_hosts: Dict,
                        host_index: Dict[str, Host],
                        configs: Dict[str, pathlib.Path]) -> None:
    work_dir = remote_work_dir(lab_hosts)
    bundle = build_bundle()
    builder_host = remote_role_host(topology, host_index, "ingress")

    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        run_remote(host, host_index, f"mkdir -p {shlex.quote(work_dir)} {shlex.quote(work_dir + '/logs')} {shlex.quote(work_dir + '/configs')}")
        upload_file(host, host_index, bundle, f"{work_dir}/xgw-smoke-src.tar.gz")
        upload_file(host, host_index, configs[role], f"{work_dir}/configs/{role}.conf")
        run_remote(host, host_index, f"cd {shlex.quote(work_dir)} && rm -rf src include bpf Makefile xgw && tar -xzf xgw-smoke-src.tar.gz")

    run_remote(builder_host, host_index, f"cd {shlex.quote(work_dir)} && {compile_command()}")
    download_file(builder_host, host_index, f"{work_dir}/xgw", BUILT_BINARY_PATH)

    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        upload_file(host, host_index, BUILT_BINARY_PATH, f"{work_dir}/xgw")
        run_remote(host, host_index, f"chmod +x {shlex.quote(work_dir + '/xgw')}")


def start(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path) -> None:
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path)
    work_dir = remote_work_dir(lab_hosts)

    for role in ("egress", "relay", "ingress"):
        host = remote_role_host(topology, host_index, role)
        run_remote(host, host_index, f"pkill -f {shlex.quote(work_dir + '/xgw run')} || true", check=False)
        run_remote(
            host,
            host_index,
            (
                f"cd {shlex.quote(work_dir)} && mkdir -p logs && "
                f"sh -c 'nohup ./xgw run configs/{role}.conf > logs/{role}.log 2>&1 < /dev/null & echo $! > logs/{role}.pid'"
            ),
        )
        time.sleep(2)


def status(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path) -> None:
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path)
    work_dir = remote_work_dir(lab_hosts)

    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        _, proc_out, _ = run_remote(host, host_index, f"pgrep -af {shlex.quote(work_dir + '/xgw run')} || true", check=False)
        _, log_out, _ = run_remote(host, host_index, f"tail -n 20 {shlex.quote(work_dir + '/logs/' + role + '.log')} || true", check=False)
        print(f"[{role}] {host.name} process")
        print(proc_out.strip() or "(no process)")
        print(f"[{role}] {host.name} log")
        print(log_out.strip() or "(no log)")
        print("")


def probe(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path) -> None:
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path, dynamic_port_check=False)
    work_dir = remote_work_dir(lab_hosts)
    ingress = remote_role_host(topology, host_index, "ingress")
    port = int(topology["runtime"]["tunnel_port"])
    run_remote(
        ingress,
        host_index,
        (
            f"cd {shlex.quote(work_dir)} && "
            f"rm -f logs/probe.tcpdump logs/probe.ss.before logs/probe.ss.after && "
            f"(command -v tcpdump >/dev/null 2>&1 && "
            f"timeout 8s tcpdump -ni any udp port {port} -c 8 > logs/probe.tcpdump 2>&1 &) || true; "
            f"ss -unap > logs/probe.ss.before 2>&1 || true"
        ),
        check=False,
    )
    run_remote(ingress, host_index, f"cd {shlex.quote(work_dir)} && ./xgw probe-send configs/ingress.conf 127.0.0.1:{port}")
    time.sleep(3)
    run_remote(
        ingress,
        host_index,
        f"cd {shlex.quote(work_dir)} && ss -unap > logs/probe.ss.after 2>&1 || true",
        check=False,
    )


def collect_snapshot(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path) -> Dict[str, Dict[str, str]]:
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path, dynamic_port_check=False)
    work_dir = remote_work_dir(lab_hosts)
    out: Dict[str, Dict[str, str]] = {}
    port = int(topology["runtime"]["tunnel_port"])
    log_pattern = (
        "runtime.recv|runtime.proc|runtime.packet.dispatch|runtime.forward.sent|"
        "dataplane.egress|bridge.ingress.deliver|secure_open_fail|"
        "control.before|control.after|runtime.bridge_ingress.send|runtime.bridge_egress.return|"
        "route.command|route.prewarm|route.active|route.drain"
    )
    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        _, proc_out, _ = run_remote(host, host_index, f"pgrep -af '^./xgw run configs/{role}.conf$' || true", check=False)
        _, ss_out, _ = run_remote(host, host_index, f"ss -lunp | grep -E ':{port}\\\\s' || true", check=False)
        _, log_out, _ = run_remote(
            host,
            host_index,
            (
                f"grep -E {shlex.quote(log_pattern)} {shlex.quote(work_dir + '/logs/' + role + '.log')} "
                f"2>/dev/null | tail -n 180 || tail -n 120 {shlex.quote(work_dir + '/logs/' + role + '.log')} || true"
            ),
            check=False,
        )
        out[role] = {
            "process": proc_out.strip(),
            "socket": ss_out.strip(),
            "log": log_out.strip(),
        }
    return out


def collect(topology_path: pathlib.Path, lab_hosts_path: pathlib.Path) -> Dict[str, Dict[str, str]]:
    data = collect_snapshot(topology_path, lab_hosts_path)
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path, dynamic_port_check=False)
    work_dir = remote_work_dir(lab_hosts)
    ingress = remote_role_host(topology, host_index, "ingress")
    _, tcpdump_out, _ = run_remote(ingress, host_index, f"tail -n 80 {shlex.quote(work_dir + '/logs/probe.tcpdump')} || true", check=False)
    _, ss_before, _ = run_remote(ingress, host_index, f"cat {shlex.quote(work_dir + '/logs/probe.ss.before')} || true", check=False)
    _, ss_after, _ = run_remote(ingress, host_index, f"cat {shlex.quote(work_dir + '/logs/probe.ss.after')} || true", check=False)
    data["ingress_probe"] = {
        "tcpdump": tcpdump_out.strip(),
        "ss_before": ss_before.strip(),
        "ss_after": ss_after.strip(),
    }
    return data


def evaluate_probe(before: Dict[str, Dict[str, str]], after: Dict[str, Dict[str, str]]) -> Dict[str, object]:
    evidence = []
    for role in ("ingress", "relay", "egress"):
        before_log = before.get(role, {}).get("log", "")
        after_log = after.get(role, {}).get("log", "")
        before_socket = before.get(role, {}).get("socket", "")
        after_socket = after.get(role, {}).get("socket", "")
        if "runtime.recv" in after_log and after_log != before_log:
            evidence.append(f"{role}:runtime.recv")
        if "runtime.proc" in after_log and after_log != before_log:
            evidence.append(f"{role}:runtime.proc")
        if role == "egress" and "dataplane.egress.data" in after_log and after_log != before_log:
            evidence.append("egress:dataplane.egress.data")
        if role == "egress" and "runtime.packet.dispatch role=egress" in after_log and after_log != before_log:
            evidence.append("egress:runtime.packet.dispatch")
        if after_socket != before_socket:
            evidence.append(f"{role}:socket-change")
    return {
        "ok": len(evidence) > 0,
        "evidence": evidence,
    }


def run_smoke_pipeline(topology: Dict,
                       lab_hosts_path: pathlib.Path,
                       output_dir: pathlib.Path,
                       profile: str,
                       write_sample: bool = False) -> Tuple[pathlib.Path, Dict[str, object]]:
    generate_configs_from_topology(topology, output_dir, write_sample=write_sample, profile=profile)
    resolved_topology_path = write_resolved_topology(topology, output_dir)
    result: Dict[str, object] = {}
    stage = "cleanup"
    try:
        cleanup(resolved_topology_path, lab_hosts_path)
        stage = "preflight"
        preflight_data = preflight(resolved_topology_path, lab_hosts_path)
        result["preflight"] = preflight_data
        stage = "deploy"
        deploy(resolved_topology_path, lab_hosts_path, output_dir, profile)
        stage = "start"
        start(resolved_topology_path, lab_hosts_path)
        stage = "collect_before"
        before = collect_snapshot(resolved_topology_path, lab_hosts_path)
        stage = "probe"
        probe(resolved_topology_path, lab_hosts_path)
        stage = "collect_after"
        after = collect_snapshot(resolved_topology_path, lab_hosts_path)
        result.update({
            "before": before,
            "after": after,
            "probe_result": evaluate_probe(before, after),
        })
        return resolved_topology_path, result
    except Exception as exc:
        result["failed_stage"] = stage
        result["error"] = str(exc)
        try:
            result["after"] = collect_snapshot(resolved_topology_path, lab_hosts_path)
        except Exception as collect_exc:
            result["collect_error"] = str(collect_exc)
        result["probe_result"] = {"ok": False, "evidence": []}
        return resolved_topology_path, result


def line_port_busy(topology: Dict, host_index: Dict[str, Host], port: int) -> bool:
    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        if is_port_busy(host, host_index, port):
            return True
    return False


def choose_line_port(topology: Dict, host_index: Dict[str, Host], preferred: int, used_ports: List[int]) -> int:
    candidates = [preferred, preferred + 1, preferred + 2, preferred + 10, 52830, 53830, 54830]
    for candidate in candidates:
        if candidate in used_ports:
            continue
        if not line_port_busy(topology, host_index, candidate):
            return candidate
    raise RuntimeError(f"no free multiline port from candidates {candidates}")


def build_multiline_topologies(base_topology: Dict, host_index: Dict[str, Host], line_count: int) -> List[Tuple[str, Dict]]:
    base_port = int(base_topology["runtime"]["tunnel_port"])
    used_ports: List[int] = []
    lines: List[Tuple[str, Dict]] = []
    for index in range(max(1, line_count)):
        line_id = "primary" if index == 0 else f"backup-port-{index}"
        preferred = base_port + index
        port = choose_line_port(base_topology, host_index, preferred, used_ports)
        used_ports.append(port)
        line_topology = set_line_identity(set_runtime_port(base_topology, port), line_id)
        lines.append((line_id, line_topology))
    return lines


def build_single_process_line_topologies(base_topology: Dict, line_count: int) -> List[Tuple[str, Dict]]:
    lines: List[Tuple[str, Dict]] = []
    for index in range(max(1, line_count)):
        line_id = "primary" if index == 0 else f"backup-{index}"
        lines.append((line_id, set_line_identity(base_topology, line_id)))
    return lines


def multiline_smoke(topology_path: pathlib.Path,
                    lab_hosts_path: pathlib.Path,
                    output_dir: pathlib.Path,
                    profile: str,
                    line_count: int) -> Dict[str, object]:
    base_topology, _, host_index = resolve_topology(topology_path, lab_hosts_path)
    line_results: List[Dict[str, object]] = []
    status_table: List[Dict[str, object]] = []
    for line_id, line_topology in build_multiline_topologies(base_topology, host_index, line_count):
        line_dir = output_dir / "multiline" / line_id
        print(f"[multiline] line={line_id} port={line_topology['runtime']['tunnel_port']} start")
        _, result = run_smoke_pipeline(line_topology, lab_hosts_path, line_dir, profile)
        status = compact_smoke_status(line_id, line_topology, result)
        print(f"[multiline] line={line_id} ok={status['ok']} evidence={','.join(status['evidence']) if status['evidence'] else '-'}")
        line_results.append({
            "line_id": line_id,
            "topology": line_topology,
            "result": result,
        })
        status_table.append(status)
    summary = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime()),
        "profile": profile,
        "line_count": len(line_results),
        "ok": all(bool(item["ok"]) for item in status_table),
        "status_table": status_table,
        "lines": line_results,
    }
    summary_path = write_multiline_summary(output_dir, profile, summary)
    summary["summary_path"] = str(summary_path)
    return summary


def send_route_command(topology: Dict,
                       lab_hosts: Dict,
                       host_index: Dict[str, Host],
                       line_id: str,
                       op: str) -> None:
    work_dir = remote_work_dir(lab_hosts)
    for role in ("ingress", "relay", "egress"):
        host = remote_role_host(topology, host_index, role)
        run_remote(
            host,
            host_index,
            f"cd {shlex.quote(work_dir)} && printf '%s %s\\n' {shlex.quote(op)} {shlex.quote(line_id)} > logs/route-control",
            check=True,
        )


def route_log_evidence(snapshot: Dict[str, Dict[str, str]]) -> List[str]:
    evidence: List[str] = []
    for role in ("ingress", "relay", "egress"):
        log = snapshot.get(role, {}).get("log", "")
        for marker in ("route.prewarm", "route.active.request", "route.active line=", "route.drain"):
            if marker in log:
                evidence.append(f"{role}:{marker}")
    return evidence


def single_multiline_smoke(topology_path: pathlib.Path,
                           lab_hosts_path: pathlib.Path,
                           output_dir: pathlib.Path,
                           profile: str,
                           line_count: int) -> Dict[str, object]:
    base_topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path)
    topologies = build_single_process_line_topologies(base_topology, line_count)
    configs = generate_single_multiline_configs(topologies, output_dir / "single-multiline", profile)
    resolved_topology_path = write_resolved_topology(base_topology, output_dir / "single-multiline")
    result: Dict[str, object] = {"profile": profile, "line_count": len(topologies)}
    try:
        cleanup(resolved_topology_path, lab_hosts_path)
        result["preflight"] = preflight(resolved_topology_path, lab_hosts_path)
        deploy_with_configs(base_topology, lab_hosts, host_index, configs)
        start(resolved_topology_path, lab_hosts_path)
        before = collect_snapshot(resolved_topology_path, lab_hosts_path)
        probe(resolved_topology_path, lab_hosts_path)
        primary_after = collect_snapshot(resolved_topology_path, lab_hosts_path)
        target_line = topologies[1][0] if len(topologies) > 1 else topologies[0][0]
        send_route_command(base_topology, lab_hosts, host_index, target_line, "PREWARM_LINE")
        time.sleep(2)
        send_route_command(base_topology, lab_hosts, host_index, target_line, "SET_ACTIVE_LINE")
        time.sleep(4)
        switched = collect_snapshot(resolved_topology_path, lab_hosts_path)
        probe(resolved_topology_path, lab_hosts_path)
        final_after = collect_snapshot(resolved_topology_path, lab_hosts_path)
        primary_probe = evaluate_probe(before, primary_after)
        final_probe = evaluate_probe(switched, final_after)
        evidence = route_log_evidence(switched) + route_log_evidence(final_after)
        result.update({
            "ok": primary_probe["ok"] and final_probe["ok"] and any("route.active line=" in item for item in evidence),
            "target_line": target_line,
            "before": before,
            "primary_after": primary_after,
            "switched": switched,
            "final_after": final_after,
            "primary_probe": primary_probe,
            "final_probe": final_probe,
            "route_evidence": sorted(set(evidence)),
        })
    except Exception as exc:
        result["ok"] = False
        result["error"] = str(exc)
        try:
            result["after"] = collect_snapshot(resolved_topology_path, lab_hosts_path)
        except Exception as collect_exc:
            result["collect_error"] = str(collect_exc)
    summary_path = SUMMARY_DIR / f"summary-single-multiline-{profile}.json"
    SUMMARY_DIR.mkdir(parents=True, exist_ok=True)
    summary_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    result["summary_path"] = str(summary_path)
    return result


def outbound_smoke(proxy_type: str, profile: str, topology_path: pathlib.Path, lab_hosts_path: pathlib.Path) -> Dict[str, str]:
    topology, lab_hosts, host_index = resolve_topology(topology_path, lab_hosts_path, dynamic_port_check=False)
    ingress = remote_role_host(topology, host_index, "ingress")
    egress = remote_role_host(topology, host_index, "egress")
    work_dir = remote_work_dir(lab_hosts)
    if proxy_type == "socks5":
        proxy_port = choose_free_port(egress, host_index, "tcp", [22080, 23080, 24080, 25080])
        echo_tcp = choose_free_port(egress, host_index, "tcp", [22081, 23081, 24081, 25081])
        echo_udp = choose_free_port(egress, host_index, "udp", [22082, 23082, 24082, 25082])
        run_remote(egress, host_index, f"cd {shlex.quote(work_dir)} && pkill -9 -f smoke_socks5_proxy.py || true && pkill -9 -f 'smoke_tcp_udp_echo.py --tcp-port {echo_tcp}' || true && pkill -9 -f 'smoke_tcp_udp_echo.py --udp-port {echo_udp}' || true && rm -f logs/socks5.log logs/echo.log && fuser -k {proxy_port}/tcp {echo_tcp}/tcp {echo_udp}/udp 2>/dev/null || true", check=False)
        run_remote(ingress, host_index, f"cd {shlex.quote(work_dir)} && pkill -9 -f 'outbound-smoke configs/outbound-socks5.conf' || true && rm -f logs/outbound-socks5.log logs/outbound-socks5.pid logs/outbound-socks5-udp.log logs/outbound-socks5-udp.pid", check=False)
        run_remote(egress, host_index, f"cd {shlex.quote(work_dir)} && nohup python3 tools/smoke_tcp_udp_echo.py --tcp-port {echo_tcp} --udp-port {echo_udp} > logs/echo.log 2>&1 < /dev/null &", check=False)
        run_remote(egress, host_index, f"cd {shlex.quote(work_dir)} && nohup python3 tools/smoke_socks5_proxy.py --port {proxy_port} --username smoke --password secret --advertise-host {egress.host} > logs/socks5.log 2>&1 < /dev/null &", check=False)
        time.sleep(3)
        wait_remote_port(egress, host_index, echo_tcp, "tcp", 10)
        wait_remote_port(egress, host_index, echo_udp, "udp", 10)
        wait_remote_port(egress, host_index, proxy_port, "tcp", 10)
        conf = (
            "node_name=outbound-socks5\n"
            "transport=udp\n"
            "proxy_mode=generic-proxy\n"
            "acl_mode=allow\n"
            "outbound_type=socks5\n"
            f"outbound_host={egress.host}\n"
            f"outbound_port={proxy_port}\n"
            "outbound_username=smoke\n"
            "outbound_password=secret\n"
            "pool_select=best\n"
            "mtu_profile=relay-balanced\n"
            "payload_profile=balanced\n"
            "hop_name=gz-141\n"
            "listen_host=0.0.0.0\n"
            "tun_name=xgw-proxy-ing\n"
            "tun_addr=10.99.0.1/24\n"
            "device=eth0\n"
            "queue_id=0\n"
            "role=ingress\n"
            f"profile={profile}\n"
            f"congestion={profile.replace('live-', '')}\n"
            "bbr_profile=standard\n"
            "auth_token=proxy-test\n"
            "advertised_rx_bps=0\n"
            "advertised_tx_bps=0\n"
            "initial_stream_receive_window=8388608\n"
            "max_stream_receive_window=8388608\n"
            "initial_connection_receive_window=20971520\n"
            "max_connection_receive_window=20971520\n"
            "max_idle_timeout_sec=30\n"
            "keepalive_sec=10\n"
            "disable_path_mtu_discovery=false\n"
            "enable_udp=true\n"
            "path=phone@mobile=0.0.0.0:0,gz-141@ingress=106.75.141.139:51830,hk-2@relay=172.20.174.36:51830,kz-1@egress=2.135.147.71:51830\n"
            "mtu=1360\npayload_size=1180\nreorder_window=128\nfec_data_shards=4\nfec_parity_shards=2\npacing_rate_bps=200000000\npacing_interval_us=50\nredundant_copies=2\n"
            "policy_group=default\nallow_cidrs=0.0.0.0/0\nallow_domains=live.tiktok.com\nallow_domain_suffixes=tiktokcdn.com\nconservative_domain_allow=true\ndynamic_grace_period_sec=30\n"
            "dos_enabled=true\nmax_connections=1000\nmax_connections_per_ip=200\nrate_limit_per_second=2000\nrate_limit_burst=2000\nblacklist_duration_sec=300\nwhitelist_ips=127.0.0.1\n"
            "obfs_mode=none\nobfs_scope=hop\nobfs_key=\n"
        )
        local_conf = TMP_DIR / "outbound-socks5.conf"
        local_conf.write_text(conf, encoding="utf-8")
        upload_file(ingress, host_index, local_conf, f"{work_dir}/configs/outbound-socks5.conf")
        start_remote_background(
            ingress,
            host_index,
            work_dir,
            f"./xgw outbound-smoke configs/outbound-socks5.conf tcp 127.0.0.1:{echo_tcp}",
            "logs/outbound-socks5.log",
            "logs/outbound-socks5.pid",
        )
        tcp_task = wait_remote_task(
            ingress,
            host_index,
            work_dir,
            "logs/outbound-socks5.log",
            "logs/outbound-socks5.pid",
            ["outbound-smoke ok proto=tcp"],
            ["outbound-smoke error:"],
            timeout_sec=40,
        )
        socks5_inspect = inspect_remote_background(
            ingress,
            host_index,
            work_dir,
            "logs/outbound-socks5.log",
            "logs/outbound-socks5.pid",
        )
        start_remote_background(
            ingress,
            host_index,
            work_dir,
            f"./xgw outbound-smoke configs/outbound-socks5.conf udp 127.0.0.1:{echo_udp}",
            "logs/outbound-socks5-udp.log",
            "logs/outbound-socks5-udp.pid",
        )
        udp_task = wait_remote_task(
            ingress,
            host_index,
            work_dir,
            "logs/outbound-socks5-udp.log",
            "logs/outbound-socks5-udp.pid",
            ["outbound-smoke ok proto=udp"],
            ["outbound-smoke error:"],
            timeout_sec=40,
        )
        socks5_udp_inspect = inspect_remote_background(
            ingress,
            host_index,
            work_dir,
            "logs/outbound-socks5-udp.log",
            "logs/outbound-socks5-udp.pid",
        )
        _, socks5_log, _ = run_remote(egress, host_index, f"tail -n 80 {shlex.quote(work_dir + '/logs/socks5.log')} || true", check=False)
        _, echo_log, _ = run_remote(egress, host_index, f"tail -n 80 {shlex.quote(work_dir + '/logs/echo.log')} || true", check=False)
        return {
            "tcp_ok": tcp_task["ok"],
            "udp_ok": udp_task["ok"],
            "outbound_log": (tcp_task["log"] + "\n" + udp_task["log"]).strip(),
            "tcp_inspect": socks5_inspect["inspection"],
            "udp_inspect": socks5_udp_inspect["inspection"],
            "proxy_log": socks5_log.strip(),
            "echo_log": echo_log.strip(),
        }
    if proxy_type == "http":
        run_remote(
            egress,
            host_index,
            (
                f"cd {shlex.quote(work_dir)} && "
                f"pkill -9 -f smoke_http_connect_proxy.py || true && "
                f"pkill -9 -f 'smoke_tcp_udp_echo.py --tcp-port' || true && "
                f"rm -f logs/http-connect.log logs/http-connect.pid logs/echo-http.log logs/echo-http.pid && "
                f"for p in 22180 23180 24180 25180 22181 23181 24181 25181 19083; do fuser -k $p/tcp 2>/dev/null || true; fuser -k $p/udp 2>/dev/null || true; done"
            ),
            check=False,
        )
        run_remote(ingress, host_index, f"cd {shlex.quote(work_dir)} && pkill -9 -f 'outbound-smoke configs/outbound-http.conf' || true && rm -f logs/outbound-http.log logs/outbound-http.pid", check=False)
        start_remote_background(
            egress,
            host_index,
            work_dir,
            "python3 tools/smoke_tcp_udp_echo.py --tcp-port 0",
            "logs/echo-http.log",
            "logs/echo-http.pid",
        )
        start_remote_background(
            egress,
            host_index,
            work_dir,
            "python3 tools/smoke_http_connect_proxy.py --port 0",
            "logs/http-connect.log",
            "logs/http-connect.pid",
        )
        echo_tcp = wait_remote_logged_port(egress, host_index, f"{work_dir}/logs/echo-http.log", "echo.tcp.listen ", 20)
        proxy_port = wait_remote_logged_port(egress, host_index, f"{work_dir}/logs/http-connect.log", "http.listen ", 20)
        echo_ready = wait_remote_port(egress, host_index, echo_tcp, "tcp", 10)
        proxy_ready = wait_remote_port(egress, host_index, proxy_port, "tcp", 10)
        proxy_alive = wait_remote_process(egress, host_index, "smoke_http_connect_proxy.py", 5)
        echo_alive = wait_remote_process(egress, host_index, "smoke_tcp_udp_echo.py", 5)
        echo_loopback_ok = wait_remote_tcp_connect(egress, host_index, "127.0.0.1", echo_tcp, 10)
        proxy_loopback_ok = wait_remote_tcp_connect(egress, host_index, "127.0.0.1", proxy_port, 10)
        if not echo_ready or not proxy_ready or not proxy_alive or not echo_alive or not echo_loopback_ok or not proxy_loopback_ok:
            _, proxy_log, _ = run_remote(egress, host_index, f"cat {shlex.quote(work_dir + '/logs/http-connect.log')} || true", check=False)
            _, echo_log, _ = run_remote(egress, host_index, f"cat {shlex.quote(work_dir + '/logs/echo-http.log')} || true", check=False)
            proxy_inspect = inspect_remote_background(egress, host_index, work_dir, "logs/http-connect.log", "logs/http-connect.pid")
            echo_inspect = inspect_remote_background(egress, host_index, work_dir, "logs/echo-http.log", "logs/echo-http.pid")
            return {
                "tcp_ok": "false",
                "echo_ready": str(echo_ready),
                "proxy_ready": str(proxy_ready),
                "proxy_alive": str(proxy_alive),
                "echo_alive": str(echo_alive),
                "echo_loopback_ok": str(echo_loopback_ok),
                "proxy_loopback_ok": str(proxy_loopback_ok),
                "echo_port": str(echo_tcp),
                "proxy_port": str(proxy_port),
                "outbound_log": "",
                "tcp_inspect": "",
                "proxy_inspect": proxy_inspect["inspection"],
                "echo_inspect": echo_inspect["inspection"],
                "proxy_log": proxy_log.strip(),
                "echo_log": echo_log.strip(),
            }
        conf = (
            "node_name=outbound-http\n"
            "transport=udp\n"
            "proxy_mode=generic-proxy\n"
            "acl_mode=allow\n"
            "outbound_type=http\n"
            f"outbound_host={egress.host}\n"
            f"outbound_port={proxy_port}\n"
            "pool_select=best\n"
            "mtu_profile=relay-balanced\n"
            "payload_profile=balanced\n"
            "hop_name=gz-141\n"
            "listen_host=0.0.0.0\n"
            "tun_name=xgw-proxy-http-ing\n"
            "tun_addr=10.99.1.1/24\n"
            "device=eth0\n"
            "queue_id=0\n"
            "role=ingress\n"
            f"profile={profile}\n"
            "congestion=bbr\n"
            "bbr_profile=standard\n"
            "auth_token=proxy-test\n"
            "advertised_rx_bps=0\n"
            "advertised_tx_bps=0\n"
            "initial_stream_receive_window=8388608\n"
            "max_stream_receive_window=8388608\n"
            "initial_connection_receive_window=20971520\n"
            "max_connection_receive_window=20971520\n"
            "max_idle_timeout_sec=30\n"
            "keepalive_sec=10\n"
            "disable_path_mtu_discovery=false\n"
            "enable_udp=true\n"
            "path=phone@mobile=0.0.0.0:0,gz-141@ingress=106.75.141.139:51830,hk-2@relay=172.20.174.36:51830,kz-1@egress=2.135.147.71:51830\n"
            "mtu=1360\npayload_size=1180\nreorder_window=128\nfec_data_shards=4\nfec_parity_shards=1\npacing_rate_bps=50000000\npacing_interval_us=200\nredundant_copies=1\n"
            "policy_group=default\nallow_cidrs=0.0.0.0/0\nallow_domains=live.tiktok.com\nallow_domain_suffixes=tiktokcdn.com\nconservative_domain_allow=true\ndynamic_grace_period_sec=30\n"
            "dos_enabled=true\nmax_connections=1000\nmax_connections_per_ip=200\nrate_limit_per_second=2000\nrate_limit_burst=2000\nblacklist_duration_sec=300\nwhitelist_ips=127.0.0.1\n"
            "obfs_mode=none\nobfs_scope=hop\nobfs_key=\n"
        )
        local_conf = TMP_DIR / "outbound-http.conf"
        local_conf.write_text(conf, encoding="utf-8")
        upload_file(ingress, host_index, local_conf, f"{work_dir}/configs/outbound-http.conf")
        start_remote_background(
            ingress,
            host_index,
            work_dir,
            f"./xgw outbound-smoke configs/outbound-http.conf tcp 127.0.0.1:{echo_tcp}",
            "logs/outbound-http.log",
            "logs/outbound-http.pid",
        )
        tcp_task = wait_remote_task(
            ingress,
            host_index,
            work_dir,
            "logs/outbound-http.log",
            "logs/outbound-http.pid",
            ["outbound-smoke ok proto=tcp"],
            ["outbound-smoke error:"],
            timeout_sec=40,
        )
        http_inspect = inspect_remote_background(
            ingress,
            host_index,
            work_dir,
            "logs/outbound-http.log",
            "logs/outbound-http.pid",
        )
        _, proxy_log, _ = run_remote(egress, host_index, f"tail -n 80 {shlex.quote(work_dir + '/logs/http-connect.log')} || true", check=False)
        _, echo_log, _ = run_remote(egress, host_index, f"tail -n 80 {shlex.quote(work_dir + '/logs/echo-http.log')} || true", check=False)
        proxy_inspect = inspect_remote_background(egress, host_index, work_dir, "logs/http-connect.log", "logs/http-connect.pid")
        echo_inspect = inspect_remote_background(egress, host_index, work_dir, "logs/echo-http.log", "logs/echo-http.pid")
        return {
            "tcp_ok": tcp_task["ok"],
            "echo_ready": str(echo_ready),
            "proxy_ready": str(proxy_ready),
            "proxy_alive": str(proxy_alive),
            "echo_alive": str(echo_alive),
            "echo_loopback_ok": str(echo_loopback_ok),
            "proxy_loopback_ok": str(proxy_loopback_ok),
            "echo_port": str(echo_tcp),
            "proxy_port": str(proxy_port),
            "outbound_log": tcp_task["log"].strip(),
            "tcp_inspect": http_inspect["inspection"],
            "proxy_inspect": proxy_inspect["inspection"],
            "echo_inspect": echo_inspect["inspection"],
            "proxy_log": proxy_log.strip(),
            "echo_log": echo_log.strip(),
        }
    raise RuntimeError(f"unsupported proxy_type: {proxy_type}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate and orchestrate minimal xgw smoke configs")
    parser.add_argument("action", choices=["generate", "deploy", "start", "status", "probe", "collect", "cleanup", "preflight", "smoke", "multiline-smoke", "single-multiline-smoke", "socks5-smoke", "http-smoke"])
    parser.add_argument("--topology", default=str(SAMPLE_DIR / "smoke-topology.json"))
    parser.add_argument("--lab-hosts", default=str(SAMPLE_DIR / "lab-hosts.json"))
    parser.add_argument("--out", default=str(GENERATED_DIR))
    parser.add_argument("--write-sample", action="store_true")
    parser.add_argument("--profile", default="live-bbr", choices=["live-bbr", "live-brutal"])
    parser.add_argument("--line-count", type=int, default=2)
    args = parser.parse_args()

    topology_path = pathlib.Path(args.topology)
    lab_hosts_path = pathlib.Path(args.lab_hosts)
    out_dir = pathlib.Path(args.out)

    if args.action == "generate":
        generate_configs(topology_path, out_dir, write_sample=args.write_sample, profile=args.profile)
        return 0
    if args.action == "deploy":
        deploy(topology_path, lab_hosts_path, out_dir, args.profile)
        return 0
    if args.action == "start":
        start(topology_path, lab_hosts_path)
        return 0
    if args.action == "status":
        status(topology_path, lab_hosts_path)
        return 0
    if args.action == "probe":
        probe(topology_path, lab_hosts_path)
        return 0
    if args.action == "collect":
        data = collect(topology_path, lab_hosts_path)
        print(json.dumps(data, indent=2))
        return 0
    if args.action == "cleanup":
        cleanup(topology_path, lab_hosts_path)
        return 0
    if args.action == "preflight":
        data = preflight(topology_path, lab_hosts_path)
        print(json.dumps(data, indent=2))
        return 0
    if args.action == "smoke":
        topology, _, _ = resolve_topology(topology_path, lab_hosts_path)
        _, result = run_smoke_pipeline(topology, lab_hosts_path, out_dir, args.profile, write_sample=args.write_sample)
        summary_path = write_summary(topology, out_dir, args.profile, result)
        print(json.dumps(result, indent=2))
        print(f"summary_path={summary_path}")
        return 0
    if args.action == "multiline-smoke":
        summary = multiline_smoke(topology_path, lab_hosts_path, out_dir, args.profile, args.line_count)
        print(json.dumps({
            "ok": summary["ok"],
            "status_table": summary["status_table"],
            "summary_path": summary["summary_path"],
        }, indent=2))
        return 0 if summary["ok"] else 1
    if args.action == "single-multiline-smoke":
        summary = single_multiline_smoke(topology_path, lab_hosts_path, out_dir, args.profile, args.line_count)
        print(json.dumps({
            "ok": summary["ok"],
            "target_line": summary.get("target_line", ""),
            "route_evidence": summary.get("route_evidence", []),
            "primary_probe": summary.get("primary_probe", {}),
            "final_probe": summary.get("final_probe", {}),
            "summary_path": summary["summary_path"],
        }, indent=2))
        return 0 if summary["ok"] else 1
    if args.action == "socks5-smoke":
        deploy(topology_path, lab_hosts_path, out_dir, args.profile)
        result = outbound_smoke("socks5", args.profile, topology_path, lab_hosts_path)
        print(json.dumps(result, indent=2))
        return 0
    if args.action == "http-smoke":
        deploy(topology_path, lab_hosts_path, out_dir, args.profile)
        result = outbound_smoke("http", args.profile, topology_path, lab_hosts_path)
        print(json.dumps(result, indent=2))
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
