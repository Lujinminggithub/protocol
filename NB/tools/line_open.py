#!/usr/bin/env python3
"""从三机清单完成 NB 单线路初始化、资格探测、稳定部署和客户端输出。"""
from __future__ import annotations

import argparse
import copy
import datetime as dt
import hashlib
import hmac
import ipaddress
import json
import os
import pathlib
import secrets
import shutil
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
ROLES = ("entry", "middle", "exit")
ROLE_ENV = {role: f"NB_SSH_PASSWORD_{role.upper()}" for role in ROLES}
RUNTIME_SECURITY_FILES = frozenset({"ca.pem", "socks.users", "tenant.conf"} | {
    f"{role}.{suffix}" for role in ROLES for suffix in ("key", "pem")
})
GENERATED_SECURITY_FILES = RUNTIME_SECURITY_FILES | {"ca.key"}
AUXILIARY_SECURITY_FILES = frozenset({"known_hosts"})

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def _single(source: dict, key: str) -> dict:
    values = source.get(key)
    if not isinstance(values, list) or len(values) != 1:
        raise ValueError(f"{key} 必须且只能包含一台机器")
    return copy.deepcopy(values[0])


def normalize_hosts(source: dict, exit_port: int = 4443) -> tuple[dict, dict[str, str]]:
    """兼容 role 对象、machines[] 和 edges/relays/terminals 三种三机清单。"""
    if all(isinstance(source.get(role), dict) for role in ROLES):
        roles = {role: copy.deepcopy(source[role]) for role in ROLES}
    elif isinstance(source.get("machines"), list):
        roles = {}
        for machine in source["machines"]:
            role = str(machine.get("role", ""))
            if role not in ROLES or role in roles:
                raise ValueError(f"machines[] role 非法或重复: {role!r}")
            roles[role] = copy.deepcopy(machine)
        if set(roles) != set(ROLES):
            raise ValueError("machines[] 必须各包含一台 entry/middle/exit")
    elif any(key in source for key in ("edges", "relays", "terminals")):
        roles = {"entry": _single(source, "edges"),
                 "middle": _single(source, "relays"),
                 "exit": _single(source, "terminals")}
    else:
        raise ValueError("清单必须提供 entry/middle/exit、machines[] 或 edges/relays/terminals")

    credentials = {}
    normalized = {}
    for role in ROLES:
        host = roles[role]
        for key in ("host",):
            if not host.get(key):
                raise ValueError(f"{role}.{key} 不能为空")
        host.setdefault("name", f"{role}-{str(host['host']).replace('.', '-')}")
        host.setdefault("port", 22)
        host.setdefault("user", "root")
        host["port"] = int(host["port"])
        if not 1 <= host["port"] <= 65535:
            raise ValueError(f"{role}.port 非法")
        env_name = str(host.get("password_env") or ROLE_ENV[role])
        password = str(host.pop("password", "") or os.environ.get(env_name, ""))
        if password:
            credentials[env_name] = password
        host["password_env"] = env_name
        host.pop("role", None)
        normalized[role] = host

    result = {role: normalized[role] for role in ROLES}
    for key in ("build_host", "release_retention", "workers", "transport", "paths", "exits", "client"):
        if key in source:
            result[key] = copy.deepcopy(source[key])
    result.setdefault("build_host", "entry")
    result.setdefault("release_retention", 5)
    result.setdefault("workers", {"entry": 1, "middle": 1, "exit": 2})
    result.setdefault("paths", {"compile_dir": "/opt/compile", "work_dir": "/etc/NB",
                                "picoquic": "/etc/NB/third_party/picoquic",
                                "certs": "/etc/NB/certs"})
    result.setdefault("transport", {})
    result["transport"].setdefault("entry", {
        "cc": "cubic", "cwin_max_bytes": 524288, "mtu_max": 1400,
        "udp_gso": False, "udp_port_min": 20000, "udp_port_max": 21023,
        "reorder_gap": 128, "reorder_delay_us": 450000})
    result["transport"].setdefault("middle", {
        "cc": "bbr", "bbr_options": "Q0.0001:F0.25:", "mtu_max": 1400,
        "udp_gso": False, "reorder_gap": 128, "reorder_delay_us": 450000})
    result["transport"].setdefault("exit", {
        "cc": "bbr", "bbr_options": "Q0.0001:", "mtu_max": 1400, "udp_gso": False,
        "dns_servers": ["1.1.1.1", "8.8.8.8"]})
    result["transport"]["exit"].setdefault("dns_servers", ["1.1.1.1", "8.8.8.8"])
    dns_servers = result["transport"]["exit"]["dns_servers"]
    if not isinstance(dns_servers, list) or not 1 <= len(dns_servers) <= 3:
        raise ValueError("transport.exit.dns_servers must contain 1..3 IPv4 addresses")
    try:
        result["transport"]["exit"]["dns_servers"] = [str(ipaddress.IPv4Address(item)) for item in dns_servers]
    except (ipaddress.AddressValueError, TypeError) as error:
        raise ValueError("transport.exit.dns_servers must contain IPv4 addresses") from error
    if not result.get("exits"):
        result["exits"] = [{"name": f"{normalized['exit']['name']}-primary",
                            "host": normalized["exit"]["host"],
                            "port": exit_port, "weight": 1, "capacity": 512,
                            "fixed_exit": normalized["exit"]["name"]}]
    for route in result["exits"]:
        fixed_exit = route.get("fixed_exit", normalized["exit"]["name"])
        if fixed_exit != normalized["exit"]["name"] or route.get("host") != normalized["exit"]["host"]:
            raise ValueError("exits[] must target the line's fixed exit device")
        route["fixed_exit"] = normalized["exit"]["name"]
        route["port"] = exit_port
    return result, credentials


def baseline_profile(hosts: dict, line_id: str, middle_port: int = 4443,
                     exit_port: int = 4443) -> dict:
    entry, middle, exit_host = (hosts[role] for role in ROLES)
    transport = hosts["transport"]
    def selected(role: str) -> dict:
        keys = ("cc", "cwin_max_bytes", "bbr_options", "mtu_max",
                "reorder_gap", "reorder_delay_us", "dns_servers")
        return {key: transport[role][key] for key in keys if key in transport[role]}
    return {
        "schema_version": 1, "line_id": line_id, "status": "bootstrap-baseline",
        "fixed_exit": exit_host["name"],
        "active_path": [entry["name"], middle["name"], exit_host["name"]],
        "candidate_entries": [entry["name"]], "candidate_relays": [middle["name"]],
        "transport": {
            "pool_size": 1,
            "workers": copy.deepcopy(hosts["workers"]),
            "entry_middle": {"address": f"{middle.get('private_ip') or middle['host']}:{middle_port}",
                             **selected("entry")},
            "middle_exit": {"address": f"{exit_host['host']}:{exit_port}", **selected("middle")},
            "exit": selected("exit"),
            "pacing": {"media_max_burst_mtu": 2, "gso": False},
            "udp": {"control_grace_us": 120000000,
                    "port_min": int(transport["entry"].get("udp_port_min", 20000)),
                    "port_max": int(transport["entry"].get("udp_port_max", 21023))},
            "fec": {"observe": True, "active": False, "codec": "RS(k,r)", "k": 4, "r": 2},
        },
        "probe_policy": {"minimum_packets": 10000, "minimum_valid_windows": 6,
                         "load_factor": 1.25, "auto_apply": False, "requires_canary": True},
    }


def write_json(path: pathlib.Path, value: dict, private: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if private:
        try: temporary.chmod(0o600)
        except OSError: pass
    temporary.replace(path)


def run(command: list[str], env: dict, cwd: pathlib.Path = ROOT) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def _secure_security_permissions(security: pathlib.Path) -> None:
    try:
        security.chmod(0o700)
    except OSError:
        pass
    private_files = {"ca.key", "socks.users", "tenant.conf"}
    private_files.update(f"{role}.key" for role in ROLES)
    for name in private_files:
        path = security / name
        if path.is_file():
            try:
                path.chmod(0o600)
            except OSError:
                pass
    for name in ({"ca.pem", "known_hosts"} |
                 {f"{role}.pem" for role in ROLES}):
        path = security / name
        if path.is_file():
            try:
                path.chmod(0o644)
            except OSError:
                pass


def ensure_security_material(security: pathlib.Path, env: dict, runner=run) -> str:
    """Create a new bundle atomically, or reuse a complete runtime identity."""
    present = ({item.name for item in security.iterdir() if item.is_file()}
               if security.exists() else set())
    if RUNTIME_SECURITY_FILES.issubset(present):
        _secure_security_permissions(security)
        print(f"reuse existing runtime security material: {security}")
        if "ca.key" not in present:
            print("CA signing key is not local; runtime deployment remains valid, "
                  "but future certificate issuance requires the original CA key")
        return "reused"

    identity_files = present & GENERATED_SECURITY_FILES
    if identity_files:
        missing = ", ".join(sorted(RUNTIME_SECURITY_FILES - present))
        raise SystemExit("security directory is incomplete; refusing to replace an "
                         f"existing identity; missing runtime files: {missing}")

    unexpected = present - AUXILIARY_SECURITY_FILES
    if unexpected:
        raise SystemExit("security directory contains unknown files; refusing to replace it: " +
                         ", ".join(sorted(unexpected)))

    security.parent.mkdir(parents=True, exist_ok=True)
    staging = security.with_name(f".{security.name}.generate-{secrets.token_hex(6)}")
    previous = security.with_name(f".{security.name}.previous-{secrets.token_hex(6)}")
    try:
        runner([sys.executable, "tools/security_setup.py", "--out", str(staging)], env)
        generated = ({item.name for item in staging.iterdir() if item.is_file()}
                     if staging.exists() else set())
        missing = GENERATED_SECURITY_FILES - generated
        if missing:
            raise RuntimeError("generated security bundle is incomplete: " +
                               ", ".join(sorted(missing)))
        for name in AUXILIARY_SECURITY_FILES:
            source = security / name
            if source.is_file():
                shutil.copy2(source, staging / name)
        _secure_security_permissions(staging)
        if security.exists():
            security.replace(previous)
        try:
            staging.replace(security)
        except BaseException:
            if previous.exists() and not security.exists():
                previous.replace(security)
            raise
        if previous.exists():
            shutil.rmtree(previous)
        return "generated"
    finally:
        if staging.exists():
            shutil.rmtree(staging)
        if previous.exists() and security.exists():
            shutil.rmtree(previous)


def deploy_socks_with_retry(socks_port: int, env: dict, runner=run,
                            sleeper=time.sleep, delays=(5, 15)) -> None:
    """Retry the rollback-safe atomic deployment stage after transient SSH loss."""
    command = [sys.executable, "tools/deploy.py", "deploy-socks",
               "--socks-port", str(socks_port)]
    attempts = len(delays) + 1
    for attempt in range(attempts):
        try:
            runner(command, env)
            return
        except subprocess.CalledProcessError:
            if attempt + 1 >= attempts:
                raise
            delay = delays[attempt]
            print(f">>> atomic deployment attempt {attempt + 1}/{attempts} failed; "
                  f"retry in {delay}s and reuse verified uploads", flush=True)
            sleeper(delay)


def sha256_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_capture(command: list[str], env: dict) -> str:
    print("+ " + " ".join(command))
    completed = subprocess.run(command, cwd=ROOT, env=env, check=False, text=True,
                               encoding="utf-8", errors="replace",
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.stdout:
        print(completed.stdout, end="" if completed.stdout.endswith("\n") else "\n")
    if completed.returncode != 0:
        raise subprocess.CalledProcessError(completed.returncode, command, completed.stdout)
    return completed.stdout


def current_deployments(env: dict) -> dict:
    output = run_capture([sys.executable, "tools/deploy.py", "current"], env)
    match = next((line.removeprefix("CURRENT_JSON=") for line in output.splitlines()
                  if line.startswith("CURRENT_JSON=")), "")
    if not match:
        raise RuntimeError("deployment query returned no CURRENT_JSON")
    value = json.loads(match)
    return value if isinstance(value, dict) else {}


def deployment_matches(env: dict, deployment_id: str) -> bool:
    if not deployment_id:
        return False
    try:
        current = current_deployments(env)
    except Exception as error:
        print(f"checkpoint verification unavailable: {error}")
        return False
    return all(current.get(role) == deployment_id for role in ROLES)


def release_manifest_for(hosts_path: pathlib.Path, profile_path: pathlib.Path) -> dict | None:
    path = ROOT / "build" / "release-manifest.json"
    if not path.is_file():
        return None
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    topology = manifest.get("topology") or {}
    profile = manifest.get("line_profile") or {}
    if (topology.get("sha256") != sha256_file(hosts_path) or
            profile.get("sha256") != sha256_file(profile_path)):
        return None
    return manifest


def source_tree_digest() -> str:
    paths = [ROOT / "CMakeLists.txt", ROOT / "VERSION", ROOT / "scripts" / "runtri.sh"]
    paths.extend(path for path in (ROOT / "src").rglob("*") if path.is_file())
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda item: item.as_posix()):
        relative = path.relative_to(ROOT).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "big")); digest.update(relative)
        digest.update(path.read_bytes())
    return digest.hexdigest()


def checkpoint_fingerprint(hosts_path: pathlib.Path, profile_path: pathlib.Path,
                           args: argparse.Namespace) -> str:
    binary = ROOT / "build" / "nb_node"
    value = {
        "checkpoint_protocol": 2,
        "line_id": args.line_id,
        "package_mbps": args.package_mbps,
        "upstream_mbps": args.upstream_mbps,
        "downstream_mbps": args.downstream_mbps,
        "socks_port": args.socks_port,
        "middle_port": args.middle_port,
        "exit_port": args.exit_port,
        "udp_port_min": args.udp_port_min,
        "udp_port_max": args.udp_port_max,
        "instance_id": os.environ.get("NB_DEPLOY_INSTANCE", ""),
        "build_mode": args.build_mode,
        "hosts_sha256": sha256_file(hosts_path),
        "profile_sha256": sha256_file(profile_path),
        "source_tree_sha256": source_tree_digest(),
        "binary_sha256": sha256_file(binary) if args.build_mode == "binary" and binary.is_file() else "",
    }
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def resolve_instance_id(line_id: str, configured: str = "") -> str:
    instance_id = configured.strip() or f"{line_id}_1"
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
    if not 1 <= len(instance_id) <= 48 or any(ch not in allowed for ch in instance_id):
        raise ValueError("line instance_id must use 1..48 safe identifier characters")
    return instance_id


def load_checkpoint(path: pathlib.Path, fingerprint: str) -> dict:
    if path.is_file():
        try:
            existing = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            existing = {}
        if existing.get("input_fingerprint") == fingerprint:
            return existing
        if existing:
            stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            write_json(path.parent / "checkpoint-history" / f"open-checkpoint-{stamp}.json", existing)
    return {"schema_version": 1, "input_fingerprint": fingerprint, "stages": {}}


def save_checkpoint(path: pathlib.Path, checkpoint: dict, stage: str, value: dict) -> None:
    checkpoint.setdefault("stages", {})[stage] = value
    checkpoint["updated_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    write_json(path, checkpoint)


def artifacts_match(stage: dict, paths: dict[str, pathlib.Path]) -> bool:
    hashes = stage.get("sha256") if isinstance(stage, dict) else None
    return isinstance(hashes, dict) and all(
        path.is_file() and hashes.get(name) == sha256_file(path) for name, path in paths.items())


def load_or_create_client_secret(path: pathlib.Path, username: str) -> dict:
    if path.is_file():
        existing = json.loads(path.read_text(encoding="utf-8"))
        if existing.get("username") != username or not existing.get("password"):
            raise RuntimeError("existing bootstrap client secret is incompatible")
        return existing
    password = (os.environ.get("NB_SOCKS_PASSWORD") or
                os.environ.get("NB_CLIENT_PASSWORD") or secrets.token_urlsafe(18))
    secret = {"username": username, "password": password,
              "password_env": "NB_OPEN_CLIENT_PASSWORD"}
    write_json(path, secret, private=True)
    return secret


def reconcile_socks_user(path: pathlib.Path, username: str, password: str) -> bool:
    if not path.is_file():
        return False
    lines = path.read_text(encoding="ascii").splitlines()
    output: list[str] = []
    found = False
    matched = False
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            output.append(line)
            continue
        parts = stripped.split(":")
        if len(parts) != 4:
            raise RuntimeError("existing socks.users contains an invalid record")
        name, rounds_text, salt_text, digest_text = parts
        if name != username:
            output.append(line)
            continue
        if found:
            raise RuntimeError("existing socks.users contains duplicate client users")
        found = True
        output.append(line)
        try:
            rounds = int(rounds_text)
            salt = bytes.fromhex(salt_text)
            expected = bytes.fromhex(digest_text)
        except (ValueError, TypeError) as error:
            raise RuntimeError("existing socks.users contains invalid client credentials") from error
        actual = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds, 32)
        if len(salt) >= 16 and len(expected) == 32 and hmac.compare_digest(actual, expected):
            matched = True
    if matched:
        return False
    salt = secrets.token_bytes(16)
    rounds = 300_000
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds, 32)
    replacement = f"{username}:{rounds}:{salt.hex()}:{digest.hex()}"
    if found:
        output = [replacement if item.strip().startswith(username + ":") else item for item in output]
    else:
        output.append(replacement)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text("\n".join(output) + "\n", encoding="ascii")
    try:
        temporary.chmod(0o600)
    except OSError:
        pass
    temporary.replace(path)
    return True


def prepare_release(env: dict, build_mode: str) -> None:
    if build_mode == "source":
        run([sys.executable, "tools/deploy.py", "build"], env)
        return
    command = [sys.executable, "tools/deploy.py", "prepare-release"]
    print("+ " + " ".join(command))
    result = subprocess.run(command, cwd=ROOT, env=env, check=False)
    if result.returncode == 0:
        return
    if build_mode == "binary":
        raise RuntimeError("verified local binary is unavailable")
    run([sys.executable, "tools/deploy.py", "build"], env)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("hosts", type=pathlib.Path, help="三台机器清单 JSON")
    parser.add_argument("--line-id", required=True)
    parser.add_argument("--package-mbps", type=float, required=True)
    parser.add_argument("--upstream-mbps", type=float)
    parser.add_argument("--downstream-mbps", type=float)
    parser.add_argument("--socks-port", type=int, default=1080)
    parser.add_argument("--middle-port", type=int, default=int(os.environ.get("NB_MIDDLE_PORT", "4443")))
    parser.add_argument("--exit-port", type=int, default=int(os.environ.get("NB_EXIT_PORT", "4443")))
    parser.add_argument("--udp-port-min", type=int, default=int(os.environ.get("NB_SOCKS_UDP_PORT_MIN", "20000")))
    parser.add_argument("--udp-port-max", type=int, default=int(os.environ.get("NB_SOCKS_UDP_PORT_MAX", "21023")))
    parser.add_argument("--client-username", default="nbmobile")
    parser.add_argument("--output-dir", type=pathlib.Path)
    parser.add_argument("--build-mode", choices=("auto", "binary", "source"), default="source")
    parser.add_argument("--execute", action="store_true", help="实际初始化和部署；默认只生成计划")
    args = parser.parse_args()
    try:
        os.environ["NB_DEPLOY_INSTANCE"] = resolve_instance_id(
            args.line_id, os.environ.get("NB_DEPLOY_INSTANCE", ""))
    except ValueError as error:
        parser.error(str(error))
    if not 1 <= args.package_mbps <= 1000:
        parser.error("--package-mbps must be between 1 and 1000")
    args.upstream_mbps = args.package_mbps if args.upstream_mbps is None else args.upstream_mbps
    args.downstream_mbps = args.package_mbps if args.downstream_mbps is None else args.downstream_mbps
    if not 1 <= args.upstream_mbps <= 1000 or not 1 <= args.downstream_mbps <= 1000:
        parser.error("--upstream-mbps and --downstream-mbps must be between 1 and 1000")
    if not 1 <= args.socks_port <= 65535:
        raise SystemExit("--socks-port 非法")
    if any(port < 1 or port > 65535 for port in (args.socks_port, args.middle_port, args.exit_port)):
        raise SystemExit("line port is invalid")
    source = json.loads(args.hosts.resolve().read_text(encoding="utf-8"))
    hosts, credentials = normalize_hosts(source, args.exit_port)
    if args.udp_port_min < 1024 or args.udp_port_max > 65535 or args.udp_port_min > args.udp_port_max:
        raise SystemExit("UDP relay range is invalid")
    hosts["transport"]["entry"]["udp_port_min"] = args.udp_port_min
    hosts["transport"]["entry"]["udp_port_max"] = args.udp_port_max
    output = (args.output_dir or ROOT / "build" / "line-open" / args.line_id).resolve()
    security = output / "security"
    bootstrap_hosts = output / "bootstrap-hosts.json"
    bootstrap_profile = output / "bootstrap-profile.json"
    write_json(bootstrap_hosts, hosts)
    write_json(bootstrap_profile, baseline_profile(hosts, args.line_id, args.middle_port, args.exit_port))
    client_secret = load_or_create_client_secret(
        output / "bootstrap-client-secret.json", args.client_username)
    client_password = client_secret["password"]
    inventory = {
        "schema_version": 1,
        "defaults": {"headroom_ratio": 1.25,
                     "probe": {"duration": 90, "ping_samples": 30}},
        "lines": [{"line_id": args.line_id, "hosts_file": str(bootstrap_hosts),
                   "baseline_profile": str(bootstrap_profile),
                   "package_mbps": args.package_mbps,
                   "allow_custom_package": True,
                   "upstream_mbps": args.upstream_mbps,
                   "downstream_mbps": args.downstream_mbps,
                   "client": {"name": args.line_id, "port": args.socks_port,
                              "username": args.client_username,
                              "password_env": "NB_OPEN_CLIENT_PASSWORD"}}],
    }
    inventory_path = output / "provision-inventory.json"
    write_json(inventory_path, inventory)
    plan = {
        "schema_version": 2, "line_id": args.line_id, "package_mbps": args.package_mbps,
        "upstream_mbps": args.upstream_mbps, "downstream_mbps": args.downstream_mbps,
        "qualification_mbps": args.package_mbps * 1.25, "socks_port": args.socks_port,
        "instance_id": os.environ.get("NB_DEPLOY_INSTANCE", ""),
        "middle_port": args.middle_port, "exit_port": args.exit_port,
        "stages": ["pin-host-keys", "generate-security", "bootstrap-build-deploy",
                   "active-quic-probe", "stable-build-deploy", "tenant-policy-apply",
                   "client-output"],
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
    }
    write_json(output / "plan.json", plan)
    if not args.execute:
        print(f"开线计划已生成: {output / 'plan.json'}")
        print("确认三机角色、SSH 地址和内网地址后，加 --execute 执行。")
        return

    env = os.environ.copy(); env.update(credentials)
    missing = [hosts[role]["password_env"] for role in ROLES
               if not env.get(hosts[role]["password_env"])]
    if missing:
        raise SystemExit("缺少 SSH 密码环境变量: " + ", ".join(missing))
    env.update({"NB_HOSTS_FILE": str(bootstrap_hosts),
                "NB_LINE_PROFILE_FILE": str(bootstrap_profile),
                "NB_SECURITY_DIR": str(security),
                 "NB_SOCKS_USERNAME": args.client_username,
                 "NB_SOCKS_PASSWORD": client_password,
                 "NB_SOCKS_PORT": str(args.socks_port),
                 "NB_MIDDLE_PORT": str(args.middle_port),
                "NB_EXIT_PORT": str(args.exit_port),
                "NB_OPEN_CLIENT_PASSWORD": client_password,
                "NB_SSH_INSECURE": "1"})
    ensure_security_material(security, env)
    if reconcile_socks_user(security / "socks.users", args.client_username, client_password):
        print("reconciled socks.users with the existing bootstrap client secret")
    known_hosts = security / "known_hosts"
    if not known_hosts.exists():
        env.pop("NB_KNOWN_HOSTS", None)
        run([sys.executable, "tools/pin_host_keys.py", "--out", str(known_hosts)], env)
    else:
        print(f"复用已有 known_hosts: {known_hosts}")
    env.pop("NB_SSH_INSECURE", None); env["NB_KNOWN_HOSTS"] = str(security / "known_hosts")
    provision_dir = output / "provision"
    line_dir = provision_dir / args.line_id
    qualified_paths = {
        "hosts": line_dir / "deployment-hosts.json",
        "profile": line_dir / "stable-profile.json",
        "client": line_dir / "client.json",
    }
    checkpoint_path = output / "open-checkpoint.json"
    fingerprint = checkpoint_fingerprint(bootstrap_hosts, bootstrap_profile, args)
    checkpoint = load_checkpoint(checkpoint_path, fingerprint)
    stages = checkpoint.setdefault("stages", {})

    stable_stage = stages.get("stable_deploy") or {}
    stable_deployment = str(stable_stage.get("deployment_id") or "")
    stable_online = deployment_matches(env, stable_deployment) if stable_deployment else False
    qualification_valid = artifacts_match(stages.get("qualification") or {}, qualified_paths)

    if stable_online and qualification_valid:
        print(f">>> resume: stable deployment {stable_deployment} already active; "
              "skip bootstrap, qualification and stable deployment")
    else:
        if not qualification_valid:
            bootstrap_stage = stages.get("bootstrap_deploy") or {}
            bootstrap_deployment = str(bootstrap_stage.get("deployment_id") or "")
            bootstrap_online = deployment_matches(env, bootstrap_deployment) if bootstrap_deployment else False
            if not bootstrap_online:
                prepare_release(env, args.build_mode)
                deploy_socks_with_retry(args.socks_port, env)
                manifest = release_manifest_for(bootstrap_hosts, bootstrap_profile)
                bootstrap_deployment = str((manifest or {}).get("deployment_id") or "")
                if not bootstrap_deployment or not deployment_matches(env, bootstrap_deployment):
                    raise RuntimeError("bootstrap deployment completed but verification failed")
                save_checkpoint(checkpoint_path, checkpoint, "bootstrap_deploy", {
                    "status": "complete", "deployment_id": bootstrap_deployment,
                    "adopted": False})
            else:
                print(f">>> resume: bootstrap deployment {bootstrap_deployment} already active")

            run([sys.executable, "tools/line_provision.py", str(inventory_path),
                 "--output-dir", str(provision_dir),
                 "--allow-conservative-fallback"], env)
            if not all(path.is_file() for path in qualified_paths.values()):
                raise RuntimeError("qualification completed without stable artifacts")
            save_checkpoint(checkpoint_path, checkpoint, "qualification", {
                "status": "complete",
                "sha256": {name: sha256_file(path) for name, path in qualified_paths.items()},
            })
            qualification_valid = True
        else:
            print(">>> resume: qualification artifacts verified; skip active probe")

        env["NB_HOSTS_FILE"] = str(qualified_paths["hosts"])
        env["NB_LINE_PROFILE_FILE"] = str(qualified_paths["profile"])
        prepare_release(env, args.build_mode)
        deploy_socks_with_retry(args.socks_port, env)
        manifest = release_manifest_for(qualified_paths["hosts"], qualified_paths["profile"])
        stable_deployment = str((manifest or {}).get("deployment_id") or "")
        if not stable_deployment or not deployment_matches(env, stable_deployment):
            raise RuntimeError("stable deployment completed but verification failed")
        stable_online = True
        save_checkpoint(checkpoint_path, checkpoint, "stable_deploy", {
            "status": "complete", "deployment_id": stable_deployment})

    env["NB_HOSTS_FILE"] = str(qualified_paths["hosts"])
    env["NB_LINE_PROFILE_FILE"] = str(qualified_paths["profile"])
    bundle = line_dir / "policy-bundle"
    policy_stage = stages.get("policy_apply") or {}
    if policy_stage.get("deployment_id") == stable_deployment and stable_online:
        print(">>> resume: tenant policy already applied")
    else:
        env["NB_CONTROL_SIGNING_KEY"] = secrets.token_hex(32)
        run([sys.executable, "tools/nb_p1_control.py", "prepare",
             str(line_dir / "tenant-route-policy.json"), str(bundle)], env)
        run([sys.executable, "tools/nb_p1_control.py", "apply", str(bundle),
             "--execute", "--socks-port", str(args.socks_port)], env)
        save_checkpoint(checkpoint_path, checkpoint, "policy_apply", {
            "status": "complete", "deployment_id": stable_deployment})
    print(f"线路开通完成；客户端配置: {line_dir / 'client.json'}")


if __name__ == "__main__":
    main()
