#!/usr/bin/env python3
"""判断回程 gap 12s 是流开头(我们的瓶颈)还是流中后段(上游应用层节奏)。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ing = df.load_hosts()["ingress"]
    cmd = r"""set +e; FRONT=$(pgrep -f xgw-edge-server|head -1); tail -c 1500000 /proc/$FRONT/fd/1 2>/dev/null"""
    _, raw, _ = df.run_remote(ing, cmd, check=False, timeout=90)
    import json, re
    early_big = 0   # gap>=1s 且 chunks<=2 (流开头就卡 = 我们的问题)
    late_big = 0    # gap>=1s 且 chunks>3 (流中后段 = 上游节奏)
    samples_early = []
    samples_late = []
    total_gap_big = 0
    for line in raw.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            o = json.loads(line)
        except Exception:
            continue
        msg = o.get("msg", "")
        if "direction=bridge_to_front" not in msg or "tiktok" not in msg:
            continue
        mg = re.search(r"gap_ms=(-?\d+)", msg)
        mc = re.search(r"chunks=(\d+)", msg)
        mt = re.search(r"total_bytes=(\d+)", msg)
        mtg = re.search(r"target=([^ ]+)", msg)
        if not (mg and mc):
            continue
        gap = int(mg.group(1)); chunks = int(mc.group(1))
        if gap < 1000:
            continue
        total_gap_big += 1
        total = int(mt.group(1)) if mt else 0
        tgt = mtg.group(1) if mtg else "?"
        if chunks <= 2:
            early_big += 1
            if len(samples_early) < 6:
                samples_early.append(f"  chunks={chunks} total={total} gap={gap}ms {tgt}")
        else:
            late_big += 1
            if len(samples_late) < 6:
                samples_late.append(f"  chunks={chunks} total={total} gap={gap}ms {tgt}")
    print(f"=== 回程 gap>=1s 共 {total_gap_big} 个 ===")
    print(f"流开头卡(chunks<=2, 我们的瓶颈): {early_big}")
    for s in samples_early: print(s)
    print(f"流中后段(chunks>3, 上游应用层节奏): {late_big}")
    for s in samples_late: print(s)
    print(f"\n判读: 若大头是'流中后段'→gap是TikTok上游节奏(非我们瓶颈)，first_byte才是关键(已400ms)；")
    print("      若大头是'流开头'→回程首段就卡，是我们的问题，需继续定位接收侧滞留。")


if __name__ == "__main__":
    main()
