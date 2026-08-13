#!/usr/bin/env python3
"""NB 线路 candidate 审批、canary 发布、回读和自动回滚。"""
from __future__ import annotations

import argparse
import copy
import datetime as dt
import hashlib
import hmac
import json
import os
import pathlib
import subprocess
import sys
import threading
import time

import line_probe


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_HOSTS = ROOT / "tools" / "lab-hosts.json"
DEFAULT_PROFILE = ROOT / "build" / "line-profiles" / "active.json"


def _canonical(value: dict) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _resign(document: dict) -> None:
    document.pop("signature_hmac_sha256", None)
    document["signature_hmac_sha256"] = hmac.new(
        _signing_key(), _canonical(document), hashlib.sha256).hexdigest()


def _sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _signing_key() -> bytes:
    key = os.environ.get("NB_PROFILE_SIGNING_KEY", "").encode("utf-8")
    if len(key) < 32: raise ValueError("NB_PROFILE_SIGNING_KEY 至少需要 32 字节")
    return key


def validate_candidate(candidate: dict) -> dict:
    if candidate.get("schema_version") != 2 or candidate.get("status") != "candidate":
        raise ValueError("只接受 line_probe schema v2 candidate")
    if candidate.get("probe_mode") != "active-quic":
        raise ValueError("candidate 必须来自主动三跳 QUIC 探针")
    if candidate.get("baseline_hosts_sha256") != _sha256(DEFAULT_HOSTS) or candidate.get("baseline_profile_sha256") != _sha256(DEFAULT_PROFILE):
        raise ValueError("candidate 对应的基线已经漂移")
    active = candidate.get("active_probe") or {}
    admission = candidate.get("admission") or {}
    if admission.get("status") != "admitted":
        raise ValueError(f"线路准入未通过: {admission.get('reasons') or ['unknown']}")
    uplink=active.get("uplink") or active.get("load") or {}
    downlink=active.get("downlink") or uplink
    if ((active.get("integrity") or {}).get("integrity") != "ok" or
        uplink.get("integrity") != "count-ok" or downlink.get("integrity") != "count-ok"):
        raise ValueError("主动探针完整性未通过")
    proposed = {}
    for segment_name, role in (("entry_middle", "entry"), ("middle_exit", "middle")):
        segment = (candidate.get("segments") or {}).get(segment_name) or {}
        recommendation = segment.get("candidate") or {}; quic = segment.get("quic") or {}
        if recommendation.get("confidence") != "load-qualified":
            raise ValueError(f"{segment_name} 样本不足")
        if int(quic.get("packets_observed", 0)) < 10000 or int(quic.get("windows_valid", 0)) < 6:
            raise ValueError(f"{segment_name} 未达到最小样本量")
        mtu = recommendation.get("mtu_evidence") or {}
        if mtu.get("confidence") not in ("quic-and-df", "quic-proven"):
            raise ValueError(f"{segment_name} MTU 缺少 QUIC 证据")
        current = recommendation.get("current") or {}
        if recommendation.get("cc") == current.get("cc") == "cubic":
            if int(recommendation.get("cwin_max_bytes", 0) or 0) < int(current.get("cwin_max_bytes", 0) or 0):
                raise ValueError(f"{segment_name}: CUBIC cwin limit cannot be reduced by canary")
        proposed[role] = {key: recommendation.get(key) for key in
            ("cc", "cwin_max_bytes", "reorder_gap", "reorder_delay_us", "mtu_max")
            if recommendation.get(key) is not None}
    return proposed


def approve(candidate_path: pathlib.Path, output: pathlib.Path, approver: str) -> dict:
    if not approver or any(ch in approver for ch in "\r\n\0"): raise ValueError("审批人不能为空")
    candidate = json.loads(candidate_path.read_text(encoding="utf-8"))
    proposed = validate_candidate(candidate)
    document = {
        "schema_version": 1,
        "state": "approved",
        "approval_id": hashlib.sha256(candidate_path.read_bytes()).hexdigest()[:16],
        "candidate_sha256": _sha256(candidate_path),
        "approved_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "approved_by": approver,
        "line": candidate["line"],
        "fixed_exit": candidate["fixed_exit"],
        "proposed_transport": proposed,
        "baseline_hosts_sha256": _sha256(DEFAULT_HOSTS),
        "baseline_profile_sha256": _sha256(DEFAULT_PROFILE),
    }
    _resign(document)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return document


def verify_approval(document: dict) -> None:
    signature = document.get("signature_hmac_sha256", "")
    unsigned = {key: value for key, value in document.items() if key != "signature_hmac_sha256"}
    expected = hmac.new(_signing_key(), _canonical(unsigned), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(signature, expected): raise ValueError("审批签名无效")
    if document.get("state") != "approved": raise ValueError("审批工件状态不是 approved")
    if document.get("baseline_hosts_sha256") != _sha256(DEFAULT_HOSTS): raise ValueError("基线拓扑已漂移")
    if document.get("baseline_profile_sha256") != _sha256(DEFAULT_PROFILE): raise ValueError("基线 profile 已漂移")


def prepare_canary(approval_path: pathlib.Path, output_dir: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, dict]:
    approval = json.loads(approval_path.read_text(encoding="utf-8"));verify_approval(approval)
    hosts = json.loads(DEFAULT_HOSTS.read_text(encoding="utf-8"))
    profile = json.loads(DEFAULT_PROFILE.read_text(encoding="utf-8"))
    for role, values in approval["proposed_transport"].items():
        target=hosts.setdefault("transport", {}).setdefault(role, {});target.update(values)
        if values.get("cc")!="cubic":target.pop("cwin_max_bytes",None)
    profile["schema_version"] = int(profile.get("schema_version", 0)) + 1
    profile["status"] = "canary"
    profile["approval_id"] = approval["approval_id"]
    profile["transport"]["entry_middle"].update(approval["proposed_transport"]["entry"])
    profile["transport"]["middle_exit"].update(approval["proposed_transport"]["middle"])
    if approval["proposed_transport"]["entry"].get("cc")!="cubic":profile["transport"]["entry_middle"].pop("cwin_max_bytes",None)
    output_dir.mkdir(parents=True, exist_ok=True)
    hosts_path = output_dir / "lab-hosts.canary.json";profile_path = output_dir / "line-profile.canary.json"
    hosts_path.write_text(json.dumps(hosts, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    profile_path.write_text(json.dumps(profile, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    state = {"schema_version": 1, "state": "canary-ready", "approval_id": approval["approval_id"],
             "created_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
             "hosts": str(hosts_path), "profile": str(profile_path),
             "hosts_sha256": _sha256(hosts_path), "profile_sha256": _sha256(profile_path)}
    _resign(state)
    (output_dir / "state.json").write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return hosts_path, profile_path, state


def verify_canary_bundle(state: dict) -> None:
    signature=state.get("signature_hmac_sha256","");unsigned={k:v for k,v in state.items() if k!="signature_hmac_sha256"}
    if not hmac.compare_digest(signature,hmac.new(_signing_key(),_canonical(unsigned),hashlib.sha256).hexdigest()):
        raise ValueError("canary bundle 签名无效")
    hosts=pathlib.Path(state["hosts"]);profile=pathlib.Path(state["profile"])
    if _sha256(hosts)!=state.get("hosts_sha256") or _sha256(profile)!=state.get("profile_sha256"):
        raise ValueError("canary 配置文件已被修改")


def evaluate_canary(snapshots: list[dict]) -> dict:
    if len(snapshots) < 6: return {"accepted": False, "reason": "insufficient-windows", "windows": len(snapshots)}
    critical = [alert for snapshot in snapshots for alert in snapshot.get("alerts", [])
        if alert.get("severity") == "critical" and not
        (alert.get("code") == "node-unreachable" and int(alert.get("streak", 0) or 0) < 2)]
    queue_ages = []
    for snapshot in snapshots:
        for worker in snapshot.get("workers", []):
            ages = (worker.get("metrics") or {}).get("queue_age_max_us", {})
            queue_ages.append(max((int(ages.get(k, 0)) for k in ("down", "up", "q2t")), default=0))
    ordered = sorted(queue_ages);p95 = ordered[min(len(ordered)-1, max(0, int(len(ordered)*0.95)))] if ordered else 0
    accepted = not critical and p95 <= 500000
    return {"accepted": accepted, "reason": "ok" if accepted else ("critical-alert" if critical else "queue-p95"),
            "windows": len(snapshots), "queue_age_p95_us": p95, "critical_alerts": len(critical)}


def verify_readback(snapshot: dict, profile: dict) -> None:
    if snapshot.get("collection_errors"): raise ValueError("canary 参数回读存在不可达节点")
    expected_id = str(profile["line_id"]);expected_schema = int(profile["schema_version"])
    workers = snapshot.get("workers", [])
    if not workers: raise ValueError("canary 参数回读没有 worker")
    for worker in workers:
        health = worker.get("health") or {}
        if health.get("line_profile") != expected_id or int(health.get("line_profile_schema", -1)) != expected_schema:
            raise ValueError(f"canary 参数回读不一致: {worker.get('role')}:{worker.get('worker')} {health}")


def _run_deploy(hosts: pathlib.Path, profile: pathlib.Path) -> None:
    env = os.environ.copy();env["NB_HOSTS_FILE"] = str(hosts);env["NB_LINE_PROFILE_FILE"] = str(profile)
    subprocess.run([sys.executable, str(ROOT / "tools" / "deploy.py"), "build"], env=env, check=True)
    subprocess.run([sys.executable, str(ROOT / "tools" / "deploy.py"), "deploy-socks"], env=env, check=True)


def _run_active_probe(result: dict, stop_event: threading.Event, duration: int) -> None:
    try:
        entry = line_probe.deploy._role_host("entry")
        socks_port = int(os.environ.get("NB_CANARY_SOCKS_PORT", "1080"))
        target_mbps = float(os.environ.get("NB_CANARY_TARGET_MBPS", "10"))
        result["integrity"] = line_probe.run_integrity_probe(entry["host"], socks_port)
        result["uplink"] = line_probe.run_load_probe(
            entry["host"], socks_port, target_mbps, duration, stop_event)
        result["downlink"] = line_probe.run_downlink_probe(
            entry["host"], socks_port, target_mbps, duration)
    except BaseException as error:
        result["error"] = f"{type(error).__name__}: {error}"


def _verify_active_probe(result: dict) -> None:
    if result.get("error"):
        raise RuntimeError(f"canary active probe failed: {result['error']}")
    if (result.get("integrity") or {}).get("integrity") != "ok":
        raise RuntimeError(f"canary integrity probe failed: {result.get('integrity')}")
    if (result.get("uplink") or {}).get("integrity") != "count-ok":
        raise RuntimeError(f"canary uplink probe failed: {result.get('uplink')}")
    if (result.get("downlink") or {}).get("integrity") != "count-ok":
        raise RuntimeError(f"canary downlink probe failed: {result.get('downlink')}")


def _current_deployment() -> str:
    result=subprocess.run([sys.executable,str(ROOT/"tools"/"deploy.py"),"current"],check=True,capture_output=True,text=True)
    marker=next((line for line in result.stdout.splitlines() if line.startswith("CURRENT_JSON=")),None)
    if marker is None:raise RuntimeError("无法读取当前 deployment")
    current=json.loads(marker.split("=",1)[1]);values=set(current.values())
    if len(values)!=1:raise RuntimeError(f"三端当前 deployment 不一致: {current}")
    return values.pop()


def _run_exact_rollback(deployment_id: str) -> None:
    subprocess.run([sys.executable,str(ROOT/"tools"/"deploy.py"),"rollback-socks","--deployment-id",deployment_id],check=True)


def execute_canary(approval_path: pathlib.Path, output_dir: pathlib.Path, duration: int, interval: int) -> dict:
    if duration < 180 or interval < 10 or duration // interval < 6: raise ValueError("canary 至少 180 秒且不少于 6 个窗口")
    hosts, profile, state = prepare_canary(approval_path, output_dir)
    verify_canary_bundle(state)
    profile_data=json.loads(profile.read_text(encoding="utf-8"))
    baseline_deployment=_current_deployment();state["baseline_deployment_id"]=baseline_deployment
    activated = False;snapshots=[];readback_failures=0
    probe_result = {};probe_stop = threading.Event();probe_thread = None
    try:
        _run_deploy(hosts, profile);activated=True;state["state"]="canary-active"
        state["canary_deployment_id"] = _current_deployment()
        probe_thread = threading.Thread(target=_run_active_probe,
            args=(probe_result, probe_stop, max(10, duration - 30)), daemon=True)
        probe_thread.start()
        deadline=time.monotonic()+duration
        while time.monotonic()<deadline:
            if probe_result.get("error"): _verify_active_probe(probe_result)
            env=os.environ.copy();env["NB_HOSTS_FILE"]=str(hosts);env["NB_LINE_PROFILE_FILE"]=str(profile)
            subprocess.run([sys.executable,str(ROOT/"tools"/"nb_observe.py"),"--no-bundle"],env=env,check=True)
            latest=json.loads((ROOT/"build"/"observability"/"latest.json").read_text(encoding="utf-8"));snapshots.append(latest)
            if latest.get("collection_errors"):
                readback_failures += 1
                if readback_failures >= 2: verify_readback(latest,profile_data)
            else:
                readback_failures = 0;verify_readback(latest,profile_data)
            time.sleep(interval)
        probe_thread.join(timeout=60)
        if probe_thread.is_alive(): raise RuntimeError("canary active probe did not finish")
        _verify_active_probe(probe_result);state["active_probe"] = probe_result
        state["transient_readback_failures"] = sum(1 for item in snapshots if item.get("collection_errors"))
        verdict=evaluate_canary(snapshots);state.update(verdict)
        if not verdict["accepted"]: raise RuntimeError(f"canary 未通过: {verdict}")
        state["state"]="accepted"
    except Exception as error:
        state["state"]="rollback-required";state["error"]=str(error)
        probe_stop.set()
        if probe_thread is not None and probe_thread.is_alive(): probe_thread.join(timeout=35)
        if activated:
            _run_exact_rollback(baseline_deployment);state["state"]="rolled-back"
        raise
    finally:
        probe_stop.set()
        if probe_thread is not None and probe_thread.is_alive(): probe_thread.join(timeout=35)
        state["active_probe"] = probe_result
        state["updated_at_utc"]=dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00","Z")
        _resign(state)
        (output_dir/"state.json").write_text(json.dumps(state,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
        (output_dir/"snapshots.json").write_text(json.dumps(snapshots,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    return state


def _atomic_promote(hosts: dict, profile: dict) -> None:
    host_bytes = (json.dumps(hosts, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    profile_bytes = (json.dumps(profile, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    old_hosts = DEFAULT_HOSTS.read_bytes();old_profile = DEFAULT_PROFILE.read_bytes()
    host_tmp = DEFAULT_HOSTS.with_suffix(DEFAULT_HOSTS.suffix + ".promote.tmp")
    profile_tmp = DEFAULT_PROFILE.with_suffix(DEFAULT_PROFILE.suffix + ".promote.tmp")
    host_tmp.write_bytes(host_bytes);profile_tmp.write_bytes(profile_bytes)
    try:
        os.replace(host_tmp, DEFAULT_HOSTS)
        os.replace(profile_tmp, DEFAULT_PROFILE)
    except BaseException:
        DEFAULT_HOSTS.write_bytes(old_hosts);DEFAULT_PROFILE.write_bytes(old_profile)
        host_tmp.unlink(missing_ok=True);profile_tmp.unlink(missing_ok=True)
        raise


def promote_canary(approval_path: pathlib.Path, output_dir: pathlib.Path) -> dict:
    approval = json.loads(approval_path.read_text(encoding="utf-8"));verify_approval(approval)
    state_path = output_dir / "state.json"
    state = json.loads(state_path.read_text(encoding="utf-8"));verify_canary_bundle(state)
    if state.get("state") != "accepted" or state.get("approval_id") != approval.get("approval_id"):
        raise ValueError("only an accepted canary with the matching approval can be promoted")
    current = _current_deployment()
    if current != state.get("canary_deployment_id"):
        raise ValueError(f"current deployment changed: {current}")
    hosts = json.loads(pathlib.Path(state["hosts"]).read_text(encoding="utf-8"))
    profile = json.loads(pathlib.Path(state["profile"]).read_text(encoding="utf-8"))
    profile["status"] = "active-baseline";profile.pop("approval_id", None)
    _atomic_promote(hosts, profile)
    state["state"] = "promoted"
    state["promoted_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    state["active_hosts_sha256"] = _sha256(DEFAULT_HOSTS)
    state["active_profile_sha256"] = _sha256(DEFAULT_PROFILE)
    _resign(state)
    state_path.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return state


def main():
    parser=argparse.ArgumentParser();sub=parser.add_subparsers(dest="command",required=True)
    p=sub.add_parser("approve");p.add_argument("candidate",type=pathlib.Path);p.add_argument("output",type=pathlib.Path);p.add_argument("--approver",required=True)
    p=sub.add_parser("verify");p.add_argument("approval",type=pathlib.Path)
    p=sub.add_parser("prepare");p.add_argument("approval",type=pathlib.Path);p.add_argument("output_dir",type=pathlib.Path)
    p=sub.add_parser("execute");p.add_argument("approval",type=pathlib.Path);p.add_argument("output_dir",type=pathlib.Path);p.add_argument("--duration",type=int,default=600);p.add_argument("--interval",type=int,default=30);p.add_argument("--execute",action="store_true")
    p=sub.add_parser("promote");p.add_argument("approval",type=pathlib.Path);p.add_argument("output_dir",type=pathlib.Path);p.add_argument("--execute",action="store_true")
    args=parser.parse_args()
    if args.command=="approve": print(json.dumps(approve(args.candidate,args.output,args.approver),ensure_ascii=False,indent=2))
    elif args.command=="verify": verify_approval(json.loads(args.approval.read_text(encoding="utf-8")));print("APPROVAL PASS")
    elif args.command=="prepare": print(prepare_canary(args.approval,args.output_dir)[2])
    elif args.command=="execute":
        if not args.execute: raise SystemExit("真实 canary 必须显式提供 --execute")
        print(execute_canary(args.approval,args.output_dir,args.duration,args.interval))
    elif args.command=="promote":
        if not args.execute: raise SystemExit("promote 必须显式提供 --execute")
        print(promote_canary(args.approval,args.output_dir))


if __name__=="__main__": main()
