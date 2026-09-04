#!/usr/bin/env python3
"""仅为候选线路热更新媒体上行突发额度，并在失败时回退。"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import numbers
import os
import pathlib
import shlex
import subprocess
import sys
from typing import Any, Callable


ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
INSTANCE = "gz2-hk2-kz-00002_1"
TENANT_PATH = f"/etc/NB/instances/{INSTANCE}/tenant.conf"
ROLES = ("entry", "exit")
TARGET_RECORD = "tenant-v3 nbmobile 256 64 5000 5000 0 2500000 625000"
EXPECTED_OLD_RECORD = "tenant-v3 nbmobile 256 64 5000 5000 0 625000 625000"
SUSTAINED_RATE_KBPS = 5000


configured_instance = os.environ.setdefault("NB_DEPLOY_INSTANCE", INSTANCE)
if configured_instance != INSTANCE:
    raise RuntimeError("本脚本只允许操作 gz2-hk2-kz-00002_1")
sys.path.insert(0, str(TOOLS))

import deploy
import nb_p1_control


def render_tenant_config(original: bytes) -> bytes:
    """仅替换目标租户的既有突发额度，拒绝意外的限速策略形状。"""
    try:
        lines = original.decode("ascii").splitlines()
    except UnicodeDecodeError as error:
        raise ValueError("tenant.conf 必须是 ASCII 文件") from error
    rendered: list[str] = []
    changed = 0
    for line in lines:
        if line.strip().startswith("tenant-v3 nbmobile "):
            if line.strip() != EXPECTED_OLD_RECORD:
                raise ValueError("nbmobile tenant-v3 记录不是预期的 5Mbps/625000 基线")
            rendered.append(TARGET_RECORD)
            changed += 1
        else:
            rendered.append(line)
    if changed != 1:
        raise ValueError("tenant.conf 必须恰好包含一条目标 tenant-v3 记录")
    return ("\n".join(rendered) + "\n").encode("ascii")


def tenant_fingerprint(data: bytes) -> str:
    """按控制端的 FNV 规则计算当前 tenant-v3 文件指纹。"""
    value = 1469598103934665603
    records = 0
    for raw in data.decode("ascii").splitlines():
        fields = raw.split()
        if not fields or fields[0].startswith("#"):
            continue
        if len(fields) != 9 or fields[0] != "tenant-v3":
            raise ValueError("tenant.conf 含有不支持的记录格式")
        for byte in fields[1].encode("ascii"):
            value ^= byte
            value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        try:
            numbers = (int(fields[2]), int(fields[3]), int(fields[4]) * 1000 // 8,
                       int(fields[5]) * 1000 // 8, int(fields[7]), int(fields[8]),
                       int(fields[6]) * 1024 * 1024)
        except ValueError as error:
            raise ValueError("tenant.conf 数值字段非法") from error
        for number in numbers:
            if number < 0:
                raise ValueError("tenant.conf 数值字段非法")
            for shift in range(0, 64, 8):
                value ^= (number >> shift) & 0xFF
                value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        records += 1
    if records == 0:
        raise ValueError("tenant.conf 不能为空")
    return f"{value:016x}"


def validate_control_readback(role: str, responses: list[dict[str, Any]], expected: str) -> dict[str, Any]:
    """确认同一角色的全部控制端点都读回目标 generation。"""
    if not responses:
        raise RuntimeError(f"{role} 没有可用控制端点")
    mismatched = [index for index, response in enumerate(responses)
                  if str(response.get("fingerprint", "")).lower() != expected.lower()]
    if mismatched:
        raise RuntimeError(f"{role} tenant fingerprint 读回不一致 workers={mismatched}")
    return {"role": role, "workers": len(responses), "fingerprint": expected}


def remote_read(role: str) -> bytes:
    return nb_p1_control._remote_read(role, TENANT_PATH)


def remote_push(role: str, data: bytes, path: str) -> None:
    nb_p1_control._remote_push(role, data, path)


def invoke(role: str, command: str) -> list[dict[str, Any]]:
    return nb_p1_control.invoke_role(role, command)


def publish(role: str, source: str) -> None:
    nb_p1_control._publish(role, source, TENANT_PATH)


def stage_directory(role: str, directory: str) -> None:
    connection = deploy.connect(role)
    try:
        deploy.checked_run(connection, "install -d -m 0700 " + shlex.quote(directory), tmo=15)
    finally:
        connection.close()


def default_collector() -> dict[str, int]:
    """汇总 Entry 与 Exit 全部控制端点的媒体队列丢弃计数。"""
    total = 0
    for role in ROLES:
        code = (
            "import glob,json,socket;items=[];"
            f"paths=glob.glob('/run/nb-{INSTANCE}-{role}-*.ctl');"
            "\nfor path in paths:\n"
            " s=socket.socket(socket.AF_UNIX);s.settimeout(3);s.connect(path);s.sendall(b'metrics\\n');"
            " value=json.loads(s.recv(262144));s.close();"
            " items.append({'worker':path.rsplit('-',1)[-1].split('.',1)[0],'udp_errors':value.get('udp_errors')})\n"
            "print(json.dumps(items,separators=(',',':')))"
        )
        connection = deploy.connect(role)
        try:
            output = deploy.checked_run(connection, "python3 -c " + shlex.quote(code), tmo=20)
            try:
                workers = json.loads(output.strip().splitlines()[-1])
            except (IndexError, json.JSONDecodeError) as error:
                raise RuntimeError(f"{role} 控制端采样格式非法") from error
            if not isinstance(workers, list) or not workers:
                raise RuntimeError(f"{role} 没有可采样的控制端点")
            for index, worker in enumerate(workers):
                if not isinstance(worker, dict):
                    raise RuntimeError(f"{role}[{index}] 控制端采样格式非法")
                errors = worker.get("udp_errors")
                if not isinstance(errors, dict):
                    raise RuntimeError(f"{role}[{index}] 缺少 udp_errors")
                value = errors.get("queue_pressure_dropped")
                if type(value) is not int or value < 0:
                    raise RuntimeError(f"{role}[{index}] queue_pressure_dropped 非法")
                total += value
        finally:
            connection.close()
    return {"queue_pressure_dropped": total}


def default_runner(request: dict[str, Any]) -> dict[str, Any]:
    """调用运维明确配置的探针并只接受机器可解析的 JSON 结果。"""
    command = os.environ.get("NB_MEDIA_BURST_RUNNER", "").strip()
    if not command:
        raise RuntimeError("未配置媒体突发探针执行器")
    completed = subprocess.run(
        shlex.split(command) + [json.dumps(request, separators=(",", ":"))],
        cwd=ROOT, text=True, encoding="utf-8", errors="replace", capture_output=True, timeout=150,
    )
    if completed.returncode:
        raise RuntimeError("媒体突发探针执行失败")
    try:
        result = json.loads(completed.stdout.strip().splitlines()[-1])
    except (IndexError, json.JSONDecodeError) as error:
        raise RuntimeError("媒体突发探针未返回 JSON") from error
    if not isinstance(result, dict):
        raise RuntimeError("媒体突发探针结果格式非法")
    return result


def required_integer(evidence: dict[str, Any], field: str, label: str) -> int:
    """拒绝缺失、布尔值与非整数的机器证据字段。"""
    value = evidence.get(field)
    if type(value) is not int:
        raise RuntimeError(f"{label}缺少合法 {field}")
    return value


def queue_pressure_sample(collector: Callable[[], dict[str, Any]]) -> int:
    """只接受明确返回的队列丢弃计数，拒绝默认零值。"""
    evidence = collector()
    if not isinstance(evidence, dict):
        raise RuntimeError("队列计数采样格式非法")
    value = required_integer(evidence, "queue_pressure_dropped", "队列计数采样")
    if value < 0:
        raise RuntimeError("队列计数采样不能为负数")
    return value


def run_synthetic_validation(runner: Callable[[dict[str, Any]], dict[str, Any]],
                             collector: Callable[[], dict[str, Any]]) -> dict[str, Any]:
    """验证短时突发不增加丢弃计数，且持续流量不超过服务速率。"""
    before = queue_pressure_sample(collector)
    burst = runner({"kind": "media_burst", "rate_mbps": 12, "duration_ms": 500})
    if not isinstance(burst, dict):
        raise RuntimeError("媒体突发探针结果格式非法")
    if required_integer(burst, "planned_bps", "媒体突发探针") != 12000000:
        raise RuntimeError("媒体突发探针计划速率不正确")
    if required_integer(burst, "planned_duration_ms", "媒体突发探针") != 500:
        raise RuntimeError("媒体突发探针计划时长不正确")
    if required_integer(burst, "sent_bytes", "媒体突发探针") < 750000:
        raise RuntimeError("媒体突发探针发送字节不足")
    if required_integer(burst, "elapsed_ms", "媒体突发探针") < 500:
        raise RuntimeError("媒体突发探针实际时长不足")
    after = queue_pressure_sample(collector)
    dropped = after - before
    if dropped != 0:
        raise RuntimeError("媒体突发产生队列丢弃")
    sustained = runner({"kind": "sustained_probe", "duration_s": 90,
                        "maximum_kbps": SUSTAINED_RATE_KBPS})
    if not isinstance(sustained, dict):
        raise RuntimeError("持续探针结果格式非法")
    average_kbps = sustained.get("average_kbps")
    if (isinstance(average_kbps, bool) or not isinstance(average_kbps, numbers.Real) or
            not math.isfinite(float(average_kbps))):
        raise RuntimeError("持续探针未返回合法平均吞吐")
    duration_s = sustained.get("duration_s")
    if (isinstance(duration_s, bool) or not isinstance(duration_s, numbers.Real) or
            not math.isfinite(float(duration_s)) or duration_s < 90):
        raise RuntimeError("持续探针实际时长不足")
    if average_kbps > SUSTAINED_RATE_KBPS:
        raise RuntimeError("持续探针超过 5Mbps 服务约束")
    return {"queue_pressure_before": before, "queue_pressure_after": after,
            "queue_pressure_delta": dropped, "configured_rate_kbps": SUSTAINED_RATE_KBPS,
            "average_kbps": float(average_kbps), "duration_s": float(duration_s),
            "burst_sent_bytes": burst["sent_bytes"], "burst_elapsed_ms": burst["elapsed_ms"]}


def restore_role(role: str, old: bytes, transaction: int, root: str) -> dict[str, Any]:
    source = f"{root}/rollback-{role}.conf"
    remote_push(role, old, source)
    invoke(role, f"tenant prepare {transaction} {source}")
    publish(role, source)
    invoke(role, f"tenant commit {transaction}")
    if remote_read(role) != old:
        raise RuntimeError(f"{role} tenant.conf 回退读回不一致")
    return validate_control_readback(role, invoke(role, "tenant status"), tenant_fingerprint(old))


def apply(execute: bool, runner: Callable[[dict[str, Any]], dict[str, Any]] | None = None,
          collector: Callable[[], dict[str, Any]] | None = None) -> dict[str, Any]:
    """执行双角色 prepare/commit/readback；未执行模式不产生远端连接。"""
    if deploy.INSTANCE_WORK != f"/etc/NB/instances/{INSTANCE}":
        raise RuntimeError("部署根目录与候选实例不一致")
    if not execute:
        return {"status": "preflight", "instance": INSTANCE, "record": TARGET_RECORD,
                "rate_up_kbps": 5000, "rate_down_kbps": 5000,
                "burst_up_bytes": 2500000, "burst_down_bytes": 625000}

    originals = {role: remote_read(role) for role in ROLES}
    if originals["entry"] != originals["exit"]:
        raise ValueError("Entry/Exit 原 tenant.conf 字节不一致")
    candidate = render_tenant_config(originals["entry"])
    expected = tenant_fingerprint(candidate)
    generation = hashlib.sha256(candidate).hexdigest()[:16]
    transaction = int(generation, 16) or 1
    root = f"{deploy.INSTANCE_WORK}/configs/media-burst-{generation}"
    active_runner = default_runner if runner is None else runner
    active_collector = default_collector if collector is None else collector
    try:
        for role in ROLES:
            stage_directory(role, root)
            remote_push(role, candidate, f"{root}/tenant.conf")
        for role in ROLES:
            invoke(role, f"tenant prepare {transaction} {root}/tenant.conf")
        results = []
        for role in ROLES:
            publish(role, f"{root}/tenant.conf")
            invoke(role, f"tenant commit {transaction}")
            if remote_read(role) != candidate:
                raise RuntimeError(f"{role} tenant.conf 读回不一致")
            results.append(validate_control_readback(role, invoke(role, "tenant status"), expected))
        synthetic = run_synthetic_validation(active_runner, active_collector)
        return {"status": "committed", "instance": INSTANCE, "generation": generation,
                "record": TARGET_RECORD, "roles": results, "synthetic": synthetic}
    except Exception as original_error:
        for role in reversed(ROLES):
            try:
                invoke(role, f"tenant abort {transaction}")
            except Exception:
                pass
        rollback_transaction = (transaction + 1) & 0xFFFFFFFFFFFFFFFF or 1
        rollback_failures = []
        for role in reversed(ROLES):
            try:
                stage_directory(role, root)
                restore_role(role, originals[role], rollback_transaction, root)
            except Exception as rollback_error:
                rollback_failures.append(f"{role}:{type(rollback_error).__name__}")
        if rollback_failures:
            raise RuntimeError("tenant 热更新失败且回退不完整: " + ",".join(rollback_failures)) from original_error
        raise RuntimeError("tenant 热更新失败，已恢复旧配置") from original_error


def main() -> None:
    parser = argparse.ArgumentParser(description="候选线路媒体突发热更新")
    parser.add_argument("--execute", action="store_true", help="执行远端热更新；默认只做本地预检")
    args = parser.parse_args()
    try:
        print(json.dumps(apply(args.execute), ensure_ascii=False, separators=(",", ":")), flush=True)
    except Exception as error:
        print(json.dumps({"status": "failed", "error_type": type(error).__name__},
                         ensure_ascii=False, separators=(",", ":")), flush=True)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
