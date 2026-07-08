#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import re
import sys


DEFAULT_MARKERS = [
    ("ctrl_start", r"fec ctrl START"),
    ("ctrl_ack", r"fec ctrl ACK"),
    ("stat", r"fec v15 stat:"),
]

OPTIONAL_MARKERS = [
    ("drop", r"fec test drop"),
    ("nack", r"fec nack "),
    ("retx_send", r"fec retx send"),
    ("retx_recv", r"fec retx recv"),
    ("recover", r"fec recover block"),
]


def count_matches(text: str, pattern: str) -> int:
    return len(re.findall(pattern, text))


def main() -> int:
    ap = argparse.ArgumentParser(description="检查 NB V1.5 FEC 日志关键路径是否跑通")
    ap.add_argument("log", help="日志文件路径")
    ap.add_argument("--expect-recovery", action="store_true", help="要求出现恢复链路(drop/nack/retx/recover)")
    args = ap.parse_args()

    path = pathlib.Path(args.log)
    if not path.exists():
        print(f"[ERR] 日志不存在: {path}")
        return 2

    text = path.read_text(encoding="utf-8", errors="ignore")

    failed = False
    print(f"[INFO] 检查日志: {path}")

    for name, pat in DEFAULT_MARKERS:
        n = count_matches(text, pat)
        print(f"[INFO] {name}: {n}")
        if n == 0:
            failed = True

    for name, pat in OPTIONAL_MARKERS:
        n = count_matches(text, pat)
        print(f"[INFO] {name}: {n}")

    if args.expect_recovery:
        for name in ("drop", "nack", "retx_send", "retx_recv", "recover"):
            pat = dict(OPTIONAL_MARKERS)[name]
            n = count_matches(text, pat)
            if n == 0:
                failed = True
                print(f"[ERR] 缺少恢复链路标记: {name}")

    if failed:
        print("[ERR] V1.5 FEC 日志检查失败")
        return 1

    print("[OK] V1.5 FEC 日志检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
