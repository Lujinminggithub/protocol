#!/usr/bin/env python3
"""NB 多 worker 进程监督器；每个 worker 持有独立 picoquic 上下文。"""
from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workers", type=int, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    if not 1 <= args.workers <= 32 or not command:
        raise SystemExit("workers 必须为 1..32 且 command 不能为空")
    children: dict[int, subprocess.Popen] = {}
    stopping = False

    def start(worker_id: int) -> None:
        env = os.environ.copy();env["NB_WORKER_ID"] = str(worker_id)
        children[worker_id] = subprocess.Popen(command, env=env, close_fds=True)

    def stop(_signum, _frame) -> None:
        nonlocal stopping
        stopping = True
        for child in children.values():
            if child.poll() is None: child.terminate()

    signal.signal(signal.SIGTERM, stop);signal.signal(signal.SIGINT, stop)
    for worker_id in range(args.workers): start(worker_id)
    while children:
        for worker_id, child in list(children.items()):
            code = child.poll()
            if code is None: continue
            if stopping:
                del children[worker_id]
            else:
                print(f"worker={worker_id} exited code={code}; restarting", file=sys.stderr, flush=True)
                time.sleep(1);start(worker_id)
        time.sleep(0.2)
    return_code = 0
    for child in children.values():
        try: return_code = max(return_code, child.wait(timeout=5))
        except subprocess.TimeoutExpired: child.kill()
    raise SystemExit(return_code)


if __name__ == "__main__":
    main()
