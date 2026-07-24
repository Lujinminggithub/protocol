#!/usr/bin/env python3
"""NB 多 worker 进程监督器；每个 worker 持有独立 picoquic 上下文。"""
from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from collections import deque


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workers", type=int, required=True)
    parser.add_argument("--restart-base", type=float, default=1.0)
    parser.add_argument("--crash-limit", type=int, default=8)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    if not 1 <= args.workers <= 32 or not command or not 0.01 <= args.restart_base <= 10 or not 2 <= args.crash_limit <= 100:
        raise SystemExit("workers 必须为 1..32 且 command 不能为空")
    children: dict[int, subprocess.Popen] = {}
    started_at: dict[int, float] = {}
    backoff = {worker_id: args.restart_base for worker_id in range(args.workers)}
    crashes = {worker_id: deque() for worker_id in range(args.workers)}
    stopping = False
    reload_requested = False

    def start(worker_id: int) -> None:
        env = os.environ.copy();env["NB_WORKER_ID"] = str(worker_id)
        children[worker_id] = subprocess.Popen(command, env=env, close_fds=True)
        started_at[worker_id] = time.monotonic()

    def stop(_signum, _frame) -> None:
        nonlocal stopping
        stopping = True
        for child in children.values():
            if child.poll() is None: child.terminate()

    def reload_workers(_signum, _frame) -> None:
        nonlocal reload_requested
        reload_requested = True

    signal.signal(signal.SIGTERM, stop);signal.signal(signal.SIGINT, stop)
    if hasattr(signal, "SIGHUP"): signal.signal(signal.SIGHUP, reload_workers)
    for worker_id in range(args.workers): start(worker_id)
    while children:
        if reload_requested and not stopping:
            reload_requested = False
            for worker_id in sorted(children):
                child=children[worker_id]
                if child.poll() is None:
                    child.terminate()
                    try: child.wait(timeout=10)
                    except subprocess.TimeoutExpired: child.kill();child.wait(timeout=5)
                start(worker_id)
                print(f"worker={worker_id} rolling-reloaded", file=sys.stderr, flush=True)
        for worker_id, child in list(children.items()):
            code = child.poll()
            if code is None: continue
            if stopping:
                del children[worker_id]
            else:
                now = time.monotonic();uptime = now - started_at.get(worker_id, now)
                history = crashes[worker_id]
                while history and now-history[0] > 300: history.popleft()
                history.append(now)
                if len(history) >= args.crash_limit:
                    print(f"worker={worker_id} crash-loop count={len(history)} window=300s; supervisor exiting", file=sys.stderr, flush=True)
                    stop(signal.SIGTERM, None)
                    for process in children.values():
                        if process.poll() is None: process.terminate()
                    raise SystemExit(75)
                if uptime >= 60: backoff[worker_id] = args.restart_base
                delay = backoff[worker_id];backoff[worker_id] = min(30.0, delay * 2)
                print(f"worker={worker_id} exited code={code} uptime={uptime:.1f}s; restart_in={delay:.1f}s", file=sys.stderr, flush=True)
                time.sleep(delay);start(worker_id)
        time.sleep(0.2)
    return_code = 0
    for child in children.values():
        try: return_code = max(return_code, child.wait(timeout=5))
        except subprocess.TimeoutExpired: child.kill()
    raise SystemExit(return_code)


if __name__ == "__main__":
    main()
