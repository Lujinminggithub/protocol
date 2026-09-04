#!/usr/bin/env python3
"""仅为候选线路热更新媒体上行突发额度，并在失败时回退。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shlex
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
INSTANCE = "gz2-hk2-kz-00002_1"
TENANT_PATH = f"/etc/NB/instances/{INSTANCE}/tenant.conf"
ROLES = ("entry", "exit")
TARGET_RECORD = "tenant-v3 nbmobile 256 64 5000 5000 0 2500000 625000"
EXPECTED_OLD_RECORD = "tenant-v3 nbmobile 256 64 5000 5000 0 625000 625000"


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


def restore_role(role: str, old: bytes, transaction: int, root: str) -> dict[str, Any]:
    source = f"{root}/rollback-{role}.conf"
    remote_push(role, old, source)
    invoke(role, f"tenant prepare {transaction} {source}")
    publish(role, source)
    invoke(role, f"tenant commit {transaction}")
    if remote_read(role) != old:
        raise RuntimeError(f"{role} tenant.conf 回退读回不一致")
    return validate_control_readback(role, invoke(role, "tenant status"), tenant_fingerprint(old))


def apply(execute: bool) -> dict[str, Any]:
    """执行双角色 prepare/commit/readback；未执行模式不产生远端连接。"""
    if deploy.INSTANCE_WORK != f"/etc/NB/instances/{INSTANCE}":
        raise RuntimeError("部署根目录与候选实例不一致")
    if not execute:
        return {"status": "preflight", "instance": INSTANCE, "record": TARGET_RECORD,
                "rate_up_kbps": 5000, "rate_down_kbps": 5000,
                "burst_up_bytes": 2500000, "burst_down_bytes": 625000}

    originals = {role: remote_read(role) for role in ROLES}
    candidates = {role: render_tenant_config(data) for role, data in originals.items()}
    expected = {role: tenant_fingerprint(data) for role, data in candidates.items()}
    generation = hashlib.sha256(candidates["entry"]).hexdigest()[:16]
    transaction = int(generation, 16) or 1
    root = f"{deploy.INSTANCE_WORK}/configs/media-burst-{generation}"
    prepared: list[str] = []
    try:
        for role in ROLES:
            stage_directory(role, root)
            remote_push(role, candidates[role], f"{root}/tenant.conf")
        for role in ROLES:
            invoke(role, f"tenant prepare {transaction} {root}/tenant.conf")
            prepared.append(role)
        results = []
        for role in ROLES:
            publish(role, f"{root}/tenant.conf")
            invoke(role, f"tenant commit {transaction}")
            if remote_read(role) != candidates[role]:
                raise RuntimeError(f"{role} tenant.conf 读回不一致")
            results.append(validate_control_readback(role, invoke(role, "tenant status"), expected[role]))
        return {"status": "committed", "instance": INSTANCE, "generation": generation,
                "record": TARGET_RECORD, "roles": results}
    except Exception as original_error:
        for role in reversed(prepared):
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
