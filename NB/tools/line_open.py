#!/usr/bin/env python3
"""从三机清单完成 NB 单线路初始化、资格探测、稳定部署和客户端输出。"""
from __future__ import annotations

import argparse
import copy
import datetime as dt
import json
import os
import pathlib
import secrets
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
ROLES = ("entry", "middle", "exit")
ROLE_ENV = {role: f"NB_SSH_PASSWORD_{role.upper()}" for role in ROLES}

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
        "cc": "bbr", "bbr_options": "Q0.0001:", "mtu_max": 1400, "udp_gso": False})
    if not result.get("exits"):
        result["exits"] = [{"name": f"{normalized['exit']['name']}-primary",
                            "host": normalized["exit"]["host"],
                            "port": exit_port, "weight": 1, "capacity": 512,
                            "fixed_exit": normalized["exit"]["name"]}]
    return result, credentials


def baseline_profile(hosts: dict, line_id: str, middle_port: int = 4443,
                     exit_port: int = 4443) -> dict:
    entry, middle, exit_host = (hosts[role] for role in ROLES)
    transport = hosts["transport"]
    def selected(role: str) -> dict:
        keys = ("cc", "cwin_max_bytes", "bbr_options", "mtu_max",
                "reorder_gap", "reorder_delay_us")
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
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if private:
        try: path.chmod(0o600)
        except OSError: pass


def run(command: list[str], env: dict, cwd: pathlib.Path = ROOT) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("hosts", type=pathlib.Path, help="三台机器清单 JSON")
    parser.add_argument("--line-id", required=True)
    parser.add_argument("--package-mbps", type=float, choices=(5, 10, 15), required=True)
    parser.add_argument("--socks-port", type=int, default=1080)
    parser.add_argument("--middle-port", type=int, default=int(os.environ.get("NB_MIDDLE_PORT", "4443")))
    parser.add_argument("--exit-port", type=int, default=int(os.environ.get("NB_EXIT_PORT", "4443")))
    parser.add_argument("--udp-port-min", type=int, default=int(os.environ.get("NB_SOCKS_UDP_PORT_MIN", "20000")))
    parser.add_argument("--udp-port-max", type=int, default=int(os.environ.get("NB_SOCKS_UDP_PORT_MAX", "21023")))
    parser.add_argument("--client-username", default="nbmobile")
    parser.add_argument("--output-dir", type=pathlib.Path)
    parser.add_argument("--execute", action="store_true", help="实际初始化和部署；默认只生成计划")
    args = parser.parse_args()
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
    client_password = os.environ.get("NB_CLIENT_PASSWORD") or secrets.token_urlsafe(18)
    client_secret = {"username": args.client_username, "password": client_password,
                     "password_env": "NB_OPEN_CLIENT_PASSWORD"}
    write_json(output / "bootstrap-client-secret.json", client_secret, private=True)
    inventory = {
        "schema_version": 1,
        "defaults": {"headroom_ratio": 1.25,
                     "probe": {"duration": 90, "ping_samples": 30}},
        "lines": [{"line_id": args.line_id, "hosts_file": str(bootstrap_hosts),
                   "baseline_profile": str(bootstrap_profile),
                   "package_mbps": args.package_mbps,
                   "client": {"name": args.line_id, "port": args.socks_port,
                              "username": args.client_username,
                              "password_env": "NB_OPEN_CLIENT_PASSWORD"}}],
    }
    inventory_path = output / "provision-inventory.json"
    write_json(inventory_path, inventory)
    plan = {
        "schema_version": 1, "line_id": args.line_id, "package_mbps": args.package_mbps,
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
    run([sys.executable, "tools/security_setup.py", "--out", str(security)], env)
    run([sys.executable, "tools/pin_host_keys.py", "--out", str(security / "known_hosts")], env)
    env.pop("NB_SSH_INSECURE", None); env["NB_KNOWN_HOSTS"] = str(security / "known_hosts")
    run([sys.executable, "tools/deploy.py", "build"], env)
    run([sys.executable, "tools/deploy.py", "deploy-socks", "--socks-port", str(args.socks_port)], env)
    provision_dir = output / "provision"
    run([sys.executable, "tools/line_provision.py", str(inventory_path),
         "--output-dir", str(provision_dir)], env)
    line_dir = provision_dir / args.line_id
    env["NB_HOSTS_FILE"] = str(line_dir / "deployment-hosts.json")
    env["NB_LINE_PROFILE_FILE"] = str(line_dir / "stable-profile.json")
    run([sys.executable, "tools/deploy.py", "build"], env)
    run([sys.executable, "tools/deploy.py", "deploy-socks", "--socks-port", str(args.socks_port)], env)
    env["NB_CONTROL_SIGNING_KEY"] = secrets.token_hex(32)
    bundle = line_dir / "policy-bundle"
    run([sys.executable, "tools/nb_p1_control.py", "prepare",
         str(line_dir / "tenant-route-policy.json"), str(bundle)], env)
    run([sys.executable, "tools/nb_p1_control.py", "apply", str(bundle),
         "--execute", "--socks-port", str(args.socks_port)], env)
    print(f"线路开通完成；客户端配置: {line_dir / 'client.json'}")


if __name__ == "__main__":
    main()
