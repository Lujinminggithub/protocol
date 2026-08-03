#!/usr/bin/env python3
"""探测 NB 线路，并生成严格稳定配置或显式待复验的保守配置。"""
from __future__ import annotations

import argparse
import copy
import datetime as dt
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.parse


ROOT = pathlib.Path(__file__).resolve().parents[1]
SUPPORTED_PACKAGES = {5.0, 10.0, 15.0}
STABLE_MIN_QUALIFICATION_RATIO = 0.95
TRANSIENT_PROBE_ERRORS = ("ConnectionAbortedError", "ConnectionResetError", "TimeoutError",
                          "WinError 10053", "WinError 10054", "timed out")

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def resolve_path(value: str, inventory_dir: pathlib.Path) -> pathlib.Path:
    path = pathlib.Path(value)
    if path.is_absolute():
        return path
    local = (inventory_dir / path).resolve()
    return local if local.exists() else (ROOT / path).resolve()


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def transient_probe_failure(output: str) -> bool:
    return any(marker in output for marker in TRANSIENT_PROBE_ERRORS)


def run_streamed(command: list[str], env: dict) -> subprocess.CompletedProcess:
    process = subprocess.Popen(command, cwd=ROOT, env=env, text=True,
                               encoding="utf-8", errors="replace",
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               bufsize=1)
    lines = []
    assert process.stdout is not None
    for line in process.stdout:
        lines.append(line)
        print(line, end="", flush=True)
    return subprocess.CompletedProcess(command, process.wait(), stdout="".join(lines))


def validate_line(line: dict) -> None:
    required = ("line_id", "hosts_file", "baseline_profile", "package_mbps", "client")
    missing = [key for key in required if key not in line]
    if missing:
        raise ValueError(f"线路缺少字段: {', '.join(missing)}")
    line_id = str(line["line_id"])
    if not line_id or not all(ch.isalnum() or ch in "-_." for ch in line_id):
        raise ValueError(f"非法 line_id: {line_id!r}")
    package = float(line["package_mbps"])
    if package not in SUPPORTED_PACKAGES and not line.get("allow_custom_package", False):
        raise ValueError(f"{line_id}: 套餐仅支持 5/10/15 Mbps；自定义值需 allow_custom_package=true")
    if package < 1 or package > 1000:
        raise ValueError(f"{line_id}: package_mbps 必须在 1..1000")
    client = line["client"]
    if not client.get("username") or not client.get("password_env"):
        raise ValueError(f"{line_id}: client.username/password_env 不能为空")
    port = int(client.get("port", 1080))
    if port < 1 or port > 65535:
        raise ValueError(f"{line_id}: client.port 非法")


def qualification_rate(package_mbps: float, headroom_ratio: float) -> float:
    if headroom_ratio < 1.0 or headroom_ratio > 2.0:
        raise ValueError("headroom_ratio 必须在 1.0..2.0")
    return round(package_mbps * headroom_ratio, 3)


def conservative_candidate(hosts_path: pathlib.Path, profile_path: pathlib.Path,
                           package_mbps: float, headroom_ratio: float,
                           reason: str) -> dict:
    """Build evidence metadata when probing cannot safely tune transport values."""
    return {
        "schema_version": 2,
        "baseline_hosts_sha256": sha256(hosts_path),
        "baseline_profile_sha256": sha256(profile_path),
        "service_package": {
            "committed_mbps": package_mbps,
            "qualification_mbps": qualification_rate(package_mbps, headroom_ratio),
            "headroom_ratio": headroom_ratio,
        },
        "admission": {"status": "pending-validation", "achieved_mbps": 0,
                      "reasons": [reason]},
        "segments": {},
        "fallback_reason": reason,
    }


def assert_qualified(candidate: dict, package_mbps: float,
                     headroom_ratio: float) -> None:
    expected = qualification_rate(package_mbps, headroom_ratio)
    service = candidate.get("service_package") or {}
    actual = float(service.get("qualification_mbps", 0) or 0)
    if abs(actual - expected) > 0.01:
        raise ValueError(f"探针资格速率不匹配: expected={expected} actual={actual}")
    admission = candidate.get("admission") or {}
    if admission.get("status") != "admitted":
        raise ValueError(f"容量准入失败: {admission.get('reasons') or ['unknown']}")
    achieved = float(admission.get("achieved_mbps", 0) or 0)
    if achieved < expected * STABLE_MIN_QUALIFICATION_RATIO:
        raise ValueError(
            f"稳定配置余量不足: achieved={achieved:.3f} Mbps, "
            f"required={expected * STABLE_MIN_QUALIFICATION_RATIO:.3f} Mbps")
    for segment_name in ("entry_middle", "middle_exit"):
        segment = (candidate.get("segments") or {}).get(segment_name) or {}
        recommendation = segment.get("candidate") or {}
        quic = segment.get("quic") or {}
        if recommendation.get("confidence") != "load-qualified":
            raise ValueError(f"{segment_name}: QUIC 负载样本不足")
        if int(quic.get("packets_observed", 0)) < 10_000 or int(quic.get("windows_valid", 0)) < 6:
            raise ValueError(f"{segment_name}: 未达到最小有效窗口")
        mtu = recommendation.get("mtu_evidence") or {}
        if mtu.get("confidence") not in ("quic-and-df", "quic-proven"):
            raise ValueError(f"{segment_name}: 缺少 QUIC MTU 证据")


def selected_transport(candidate: dict, segment_name: str) -> dict:
    recommendation = candidate["segments"][segment_name]["candidate"]
    if recommendation.get("auto_apply_allowed") is not True:
        return {}
    keys = ("cc", "cwin_max_bytes", "reorder_gap", "reorder_delay_us", "mtu_max")
    selected = {key: recommendation.get(key) for key in keys
                if recommendation.get(key) is not None}
    if selected.get("cc") != "cubic":
        selected.pop("cwin_max_bytes", None)
    return selected


def client_document(line: dict, hosts: dict, password: str) -> dict:
    client = line["client"]
    host = str(client.get("host") or hosts["entry"]["host"])
    port = int(client.get("port", 1080))
    username = str(client["username"])
    label = urllib.parse.quote(str(client.get("name") or line["line_id"]), safe="")
    authority = (f"{urllib.parse.quote(username, safe='')}:{urllib.parse.quote(password, safe='')}"
                 f"@{host}:{port}")
    return {
        "type": "socks5",
        "name": str(client.get("name") or line["line_id"]),
        "server": host,
        "port": port,
        "username": username,
        "password": password,
        "shadowrocket_url": f"socks5://{authority}#{label}",
    }


def build_artifacts(line: dict, candidate: dict, hosts: dict, baseline: dict,
                    password: str, headroom_ratio: float,
                    allow_conservative_fallback: bool = False) -> dict:
    package = float(line["package_mbps"])
    qualification_error = ""
    try:
        assert_qualified(candidate, package, headroom_ratio)
    except ValueError as error:
        if not allow_conservative_fallback:
            raise
        qualification_error = str(candidate.get("fallback_reason") or error)
    qualified = not qualification_error
    generated_hosts = copy.deepcopy(hosts)
    entry_transport = selected_transport(candidate, "entry_middle") if qualified else {}
    middle_transport = selected_transport(candidate, "middle_exit") if qualified else {}
    generated_hosts.setdefault("transport", {}).setdefault("entry", {}).update(entry_transport)
    generated_hosts.setdefault("transport", {}).setdefault("middle", {}).update(middle_transport)
    generated_hosts["line_service"] = {
        "line_id": line["line_id"],
        "package_mbps": package,
        "qualification_mbps": qualification_rate(package, headroom_ratio),
        "headroom_ratio": headroom_ratio,
    }

    profile = copy.deepcopy(baseline)
    profile["schema_version"] = int(profile.get("schema_version", 0)) + 1
    profile["line_id"] = line["line_id"]
    profile["status"] = "stable-qualified" if qualified else "provisional-conservative"
    profile["generated_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    profile["source_candidate_sha256"] = hashlib.sha256(
        json.dumps(candidate, ensure_ascii=False, sort_keys=True).encode("utf-8")).hexdigest()
    profile.setdefault("transport", {}).setdefault("entry_middle", {}).update(entry_transport)
    profile.setdefault("transport", {}).setdefault("middle_exit", {}).update(middle_transport)
    profile["service_package"] = {
        "committed_mbps": package,
        "qualification_mbps": qualification_rate(package, headroom_ratio),
        "headroom_ratio": headroom_ratio,
        "measured_mbps": float((candidate.get("admission") or {}).get("achieved_mbps", 0) or 0),
        "admission": "passed" if qualified else "pending-validation",
    }
    if qualification_error:
        profile["qualification_warning"] = qualification_error

    username = str(line["client"]["username"])
    rate_kbps = int(round(package * 1000))
    routes = []
    for route in generated_hosts.get("exits", []):
        routes.append({
            "name": route["name"], "host": route["host"], "port": int(route["port"]),
            "weight": int(route.get("weight", 1)), "capacity": int(route.get("capacity", 0)),
            "fixed_exit": route.get("fixed_exit") or profile.get("fixed_exit"),
        })
    if not routes:
        raise ValueError("拓扑没有 exits[]，无法生成固定出口策略")
    policy = {
        "schema_version": 1,
        "fixed_exit": profile.get("fixed_exit"),
        "tenants": [{"name": username, "max_tcp": 256, "max_udp": 64,
                     "rate_kbps": rate_kbps, "quota_mb": 0,
                     "burst_seconds": 10}],
        "routes": routes,
    }
    return {"hosts": generated_hosts, "profile": profile, "policy": policy,
            "client": client_document(line, hosts, password),
            "qualification": {"status": "qualified" if qualified else "provisional",
                              "warning": qualification_error}}


def write_json(path: pathlib.Path, value: dict, private: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if private:
        try:
            temporary.chmod(0o600)
        except OSError:
            pass
    temporary.replace(path)


def run_probe(line: dict, inventory_dir: pathlib.Path, output: pathlib.Path,
              defaults: dict) -> tuple[dict, dict, dict]:
    hosts_path = resolve_path(line["hosts_file"], inventory_dir)
    profile_path = resolve_path(line["baseline_profile"], inventory_dir)
    if not hosts_path.is_file() or not profile_path.is_file():
        raise ValueError(f"拓扑或基线 profile 不存在: {hosts_path}, {profile_path}")
    package = float(line["package_mbps"])
    headroom = float(line.get("headroom_ratio", defaults.get("headroom_ratio", 1.25)))
    probe = {**defaults.get("probe", {}), **line.get("probe", {})}
    command = [sys.executable, str(ROOT / "tools" / "line_probe.py"), "--active",
               "--package-mbps", str(package), "--headroom-ratio", str(headroom),
               "--duration", str(int(probe.get("duration", 90))),
               "--ping-samples", str(int(probe.get("ping_samples", 30))),
               "--socks-port", str(int(line["client"].get("port", 1080))),
               "--output", str(output)]
    env = os.environ.copy()
    env["NB_HOSTS_FILE"] = str(hosts_path)
    env["NB_LINE_PROFILE_FILE"] = str(profile_path)
    env["NB_SOCKS_USERNAME"] = str(line["client"]["username"])
    password_env = str(line["client"]["password_env"])
    password = env.get(password_env, "")
    if not password:
        raise ValueError(f"缺少客户端密码环境变量: {password_env}")
    env["NB_SOCKS_PASSWORD"] = password
    completed = None
    for attempt in range(2):
        completed = run_streamed(command, env)
        if completed.returncode == 0:
            break
        if attempt == 1 or not transient_probe_failure(completed.stdout):
            raise RuntimeError(f"主动探针失败 rc={completed.returncode}:\n{completed.stdout[-4000:]}")
        print("主动探针遇到瞬时连接中断，2 秒后重试一次", file=sys.stderr)
        time.sleep(2)
    assert completed is not None
    candidate = json.loads(output.read_text(encoding="utf-8"))
    if candidate.get("baseline_hosts_sha256") != sha256(hosts_path):
        raise ValueError("探针报告与当前 hosts 文件不匹配")
    if candidate.get("baseline_profile_sha256") != sha256(profile_path):
        raise ValueError("探针报告与当前基线 profile 不匹配")
    return candidate, json.loads(hosts_path.read_text(encoding="utf-8")), \
        json.loads(profile_path.read_text(encoding="utf-8"))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("inventory", type=pathlib.Path, help="批量线路开通清单 JSON")
    parser.add_argument("--output-dir", type=pathlib.Path)
    parser.add_argument("--line", action="append", dest="selected_lines",
                        help="只执行指定 line_id，可重复")
    parser.add_argument("--allow-conservative-fallback", action="store_true",
                        help="探针不足时沿用基线参数并标记待复验；仅供开线流程使用")
    args = parser.parse_args()
    inventory_path = args.inventory.resolve()
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    if inventory.get("schema_version") != 1 or not isinstance(inventory.get("lines"), list):
        raise SystemExit("清单必须是 schema_version=1 且包含 lines[]")
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output_root = (args.output_dir or ROOT / "build" / "line-provision" / run_id).resolve()
    selected = set(args.selected_lines or [])
    summaries = []
    seen = set()
    for line in inventory["lines"]:
        line_id = str(line.get("line_id", ""))
        if selected and line_id not in selected:
            continue
        line_dir = output_root / line_id
        try:
            validate_line(line)
            if line_id in seen:
                raise ValueError(f"重复 line_id: {line_id}")
            seen.add(line_id)
            candidate_path = line_dir / "probe-candidate.json"
            try:
                candidate, hosts, baseline = run_probe(
                    line, inventory_path.parent, candidate_path, inventory.get("defaults", {}))
            except Exception as error:
                if not args.allow_conservative_fallback:
                    raise
                hosts_path = resolve_path(line["hosts_file"], inventory_path.parent)
                profile_path = resolve_path(line["baseline_profile"], inventory_path.parent)
                if not hosts_path.is_file() or not profile_path.is_file():
                    raise
                hosts = json.loads(hosts_path.read_text(encoding="utf-8"))
                baseline = json.loads(profile_path.read_text(encoding="utf-8"))
                headroom = float(line.get("headroom_ratio",
                                         inventory.get("defaults", {}).get("headroom_ratio", 1.25)))
                candidate = conservative_candidate(
                    hosts_path, profile_path, float(line["package_mbps"]), headroom, str(error))
                print(f"[{line_id}] WARNING: 主动探针未完成，使用保守基线并标记待复验: {error}",
                      file=sys.stderr)
            password = os.environ[str(line["client"]["password_env"])]
            headroom = float(line.get("headroom_ratio",
                                     inventory.get("defaults", {}).get("headroom_ratio", 1.25)))
            artifacts = build_artifacts(line, candidate, hosts, baseline, password, headroom,
                                        allow_conservative_fallback=args.allow_conservative_fallback)
            write_json(line_dir / "stable-profile.json", artifacts["profile"])
            write_json(line_dir / "deployment-hosts.json", artifacts["hosts"])
            write_json(line_dir / "tenant-route-policy.json", artifacts["policy"])
            write_json(line_dir / "client.json", artifacts["client"], private=True)
            status = artifacts["qualification"]["status"]
            summaries.append({
                "line_id": line_id, "status": status,
                "package_mbps": float(line["package_mbps"]),
                "measured_mbps": artifacts["profile"]["service_package"]["measured_mbps"],
                "profile": str(line_dir / "stable-profile.json"),
                "hosts": str(line_dir / "deployment-hosts.json"),
                "policy": str(line_dir / "tenant-route-policy.json"),
                "client": str(line_dir / "client.json"),
                "client_endpoint": f"{artifacts['client']['server']}:{artifacts['client']['port']}",
                "warning": artifacts["qualification"]["warning"],
            })
            if status == "qualified":
                print(f"[{line_id}] 通过 {line['package_mbps']} Mbps 套餐准入；客户端配置: {line_dir / 'client.json'}")
            else:
                print(f"[{line_id}] 开线使用保守默认配置，容量待复验；客户端配置: {line_dir / 'client.json'}")
        except Exception as error:
            line_dir.mkdir(parents=True, exist_ok=True)
            summaries.append({"line_id": line_id or "invalid", "status": "rejected", "error": str(error)})
            print(f"[{line_id or 'invalid'}] 拒绝开通: {error}", file=sys.stderr)
    summary = {"schema_version": 1, "run_id": run_id,
               "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
               "lines": summaries}
    write_json(output_root / "summary.json", summary)
    print(f"批量结果: {output_root / 'summary.json'}")
    accepted = {"qualified", "provisional"} if args.allow_conservative_fallback else {"qualified"}
    if not summaries or any(item["status"] not in accepted for item in summaries):
        raise SystemExit(2)


if __name__ == "__main__":
    main()
