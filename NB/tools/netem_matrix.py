#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import statistics
import time

from deploy import connect, run, _role_host
from v15_fec_test import apply_stage_env, prepare_payload_target, curl_payload


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, int(round((len(ordered) - 1) * p)))
    return ordered[idx]


def main() -> int:
    ap = argparse.ArgumentParser(description="NB V1.5 loss/netem 自动矩阵")
    ap.add_argument("--loss", default="0,1,2,3,5,8", help="逗号分隔的丢包百分比")
    ap.add_argument("--delay-ms", type=int, default=80)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--settle", type=int, default=20)
    ap.add_argument("--mode", choices=["observe", "active"], default="observe")
    ap.add_argument("--apply", action="store_true", help="确认修改 middle 出口 qdisc")
    args = ap.parse_args()
    if not args.apply:
        ap.error("netem 会修改 middle qdisc，确认后必须附加 --apply")

    losses = [float(x) for x in args.loss.split(",")]
    cm = connect("middle")
    kz = _role_host("exit")["host"]
    iface = run(cm, f"ip route get {kz} | awk '{{for(i=1;i<=NF;i++)if($i==\"dev\"){{print $(i+1);exit}}}}'").strip()
    if not iface or any(ch not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-" for ch in iface):
        cm.close()
        raise RuntimeError(f"无法识别 middle->exit 网卡: {iface!r}")
    original = run(cm, f"tc qdisc show dev {iface}").strip()
    if any(kind in original for kind in (" netem ", " htb ", " tbf ")):
        cm.close()
        raise RuntimeError(f"拒绝覆盖已有整形 qdisc: {original}")

    results: list[dict[str, object]] = []
    sha = prepare_payload_target()
    try:
        apply_stage_env("t1" if args.mode == "active" else "auto")
        for loss in losses:
            run(cm, f"tc qdisc replace dev {iface} root netem delay {args.delay_ms}ms loss {loss}%", tmo=20)
            time.sleep(args.settle)
            started = time.time()
            try:
                times = curl_payload(args.rounds, sha)
                results.append({
                    "loss_pct": loss,
                    "delay_ms": args.delay_ms,
                    "mode": args.mode,
                    "success": len(times),
                    "rounds": args.rounds,
                    "mean_s": statistics.mean(times),
                    "p50_s": percentile(times, 0.50),
                    "p95_s": percentile(times, 0.95),
                    "elapsed_s": time.time() - started,
                })
            except Exception as exc:
                results.append({"loss_pct": loss, "delay_ms": args.delay_ms, "mode": args.mode,
                    "success": 0, "rounds": args.rounds, "error": repr(exc)})
            print(json.dumps(results[-1], ensure_ascii=False))
    finally:
        run(cm, f"tc qdisc del dev {iface} root 2>/dev/null || true", tmo=20)
        cm.close()
        if args.mode == "active":
            apply_stage_env("auto")

    print(json.dumps({"iface": iface, "original_qdisc": original, "results": results}, ensure_ascii=False, indent=2))
    return 0 if all(r.get("success") == args.rounds for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
