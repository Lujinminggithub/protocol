#!/usr/bin/env python3
"""污染修复后 TikTok first_byte / 回程 gap 干净统计（JSON解析，活流+close流都算）。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ing = df.load_hosts()["ingress"]
    # 拉前端日志最后 1.5MB 到本地解析
    cmd = r"""set +e; FRONT=$(pgrep -f xgw-edge-server|head -1); tail -c 1500000 /proc/$FRONT/fd/1 2>/dev/null"""
    _, raw, _ = df.run_remote(ing, cmd, check=False, timeout=90)
    import json, re
    fb = []          # tiktok first_byte_ms (>=0)
    fb_neg = 0
    gaps = []        # bridge_to_front gap_ms for tiktok
    closes = 0
    for line in raw.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            o = json.loads(line)
        except Exception:
            continue
        msg = o.get("msg", "")
        if "tiktok" not in msg:
            continue
        if "flow.close" in msg or "trace.front.close" in msg:
            closes += 1
            m = re.search(r"first_byte_ms=(-?\d+)", msg)
            if m:
                v = int(m.group(1))
                if v < 0:
                    fb_neg += 1
                else:
                    fb.append(v)
        if "direction=bridge_to_front" in msg:
            m = re.search(r"gap_ms=(-?\d+)", msg)
            if m:
                g = int(m.group(1))
                if g >= 0:
                    gaps.append(g)

    def pct(a, p):
        if not a:
            return 0
        s = sorted(a)
        return s[min(len(s) - 1, int(len(s) * p))]

    print(f"=== TikTok flow.close: 总={closes} 有first_byte={len(fb)} -1={fb_neg} ===")
    if fb:
        print(f"  first_byte_ms: p50={pct(fb,0.5)} p90={pct(fb,0.9)} max={max(fb)} min={min(fb)}")
    print(f"=== 回程 bridge_to_front gap_ms (tiktok, n={len(gaps)}) ===")
    if gaps:
        slow = sum(1 for g in gaps if g >= 1000)
        print(f"  p50={pct(gaps,0.5)} p90={pct(gaps,0.9)} max={max(gaps)}  gap>=1s={slow}")
    print(f"\n对照: 污染修复前 first_byte p50≈1000ms(944~1981)、-1率14/96≈15%；纯hy2金标准≈几百ms")


if __name__ == "__main__":
    main()
