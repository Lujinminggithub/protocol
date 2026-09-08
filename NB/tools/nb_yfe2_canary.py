#!/usr/bin/env python3
"""NB YFE2 同编号 Middle/Exit shard 灰度事务。"""
from __future__ import annotations

import argparse
import json
from typing import Protocol


CANARY_STEPS = (
    "verify_exit_capability",
    "publish_exit_decoder",
    "restart_exit",
    "publish_middle_schema1",
    "restart_middle",
    "publish_middle_schema2",
    "verify_accepted",
)


class CanaryBackend(Protocol):
    def run(self, action: str, role: str, worker: int) -> None: ...


def paired_workers(middle_worker: int, exit_worker: int) -> int:
    if middle_worker < 0 or middle_worker > 31 or exit_worker < 0 or exit_worker > 31:
        raise ValueError("worker 编号必须在 0..31")
    if middle_worker != exit_worker:
        raise ValueError("Middle 与 Exit worker 编号必须一致")
    return middle_worker


def execute_canary(backend: CanaryBackend, middle_worker: int, exit_worker: int) -> dict:
    worker = paired_workers(middle_worker, exit_worker)
    activated_exit = False
    activated_middle = False
    try:
        for action in CANARY_STEPS:
            role = "exit" if action in {
                "verify_exit_capability", "publish_exit_decoder", "restart_exit"
            } else "middle"
            backend.run(action, role, worker)
            activated_exit |= action == "restart_exit"
            activated_middle |= action == "restart_middle"
        return {"status": "accepted", "worker": worker, "schema": 2}
    except Exception:
        # 先停止产生新 parity，再处理二进制；该顺序是事务不变量。
        try:
            backend.run("restore_middle_schema1", "middle", worker)
        finally:
            if activated_middle:
                backend.run("rollback_middle_binary", "middle", worker)
            if activated_exit:
                backend.run("rollback_exit_binary", "exit", worker)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 NB YFE2 同编号 shard 灰度计划")
    parser.add_argument("--middle-worker", type=int, required=True)
    parser.add_argument("--exit-worker", type=int, required=True)
    args = parser.parse_args()
    worker = paired_workers(args.middle_worker, args.exit_worker)
    print(json.dumps({"worker": worker, "steps": list(CANARY_STEPS)},
                     ensure_ascii=False, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
