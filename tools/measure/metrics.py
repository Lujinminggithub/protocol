"""指标层：统一口径的 per-hop / per-target 指标 + 双向逐跳归因。

统一判读口径（取代各 diag 脚本各自为政的标准）：
  - first_byte：用 close 行的 first_byte_ms（或 first_byte 事件）；-1 计入超时率，不计入分位。
  - 回程空：return_bytes==0（C 层）/ bridge_to_front_bytes==0（前端）。
  - gap 位置：gap>=1s 且 chunks<=2 = 流开头卡（我们的瓶颈）；
              gap>=1s 且 chunks>3  = 流中后段（上游应用层节奏，非我们）。
  - 双向逐跳归因：去程减物理单程得协议开销；回程逐跳归因首字节延迟。

健康基线（来自诊断结论）：回程 first_byte p50 < 660ms 为健康（物理 234ms × ~2.8）。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional

from .correlate import FlowLeg, HopFlows, legs_by_target

GAP_BIG_MS = 1000
HEALTH_FIRST_BYTE_P50_MS = 660


def pct(values: List[int], p: float) -> int:
    """统一分位函数（p in [0,1]）。"""
    if not values:
        return 0
    s = sorted(values)
    idx = min(len(s) - 1, int(len(s) * p))
    return s[idx]


@dataclass
class HopMetrics:
    hop: str
    n: int = 0
    n_timeout: int = 0
    timeout_rate: float = 0.0
    first_byte_p50: int = 0
    first_byte_p90: int = 0
    first_byte_max: int = 0
    lifetime_p50: int = 0
    lifetime_p90: int = 0
    return_zero_rate: float = 0.0  # 回程零字节流占比
    connect_p50: Optional[int] = None  # egress 上游连接耗时
    reason_dist: Dict[str, int] = field(default_factory=dict)


@dataclass
class GapClass:
    early_stall: int = 0  # gap>=1s & chunks<=2（回程首段卡 = 我们的瓶颈）
    late_pacing: int = 0  # gap>=1s & chunks>3（上游应用层节奏）
    total_big: int = 0
    samples_early: List[str] = field(default_factory=list)
    samples_late: List[str] = field(default_factory=list)


def hop_metrics(hf: HopFlows, target: Optional[str] = None) -> HopMetrics:
    legs = hf.legs
    if target is not None:
        legs = [l for l in legs if target in l.target]
    m = HopMetrics(hop=hf.hop)
    if not legs:
        return m
    fb_vals = [l.first_byte_ms for l in legs if l.first_byte_ms is not None and l.first_byte_ms >= 0]
    lifetimes = [l.lifetime_ms for l in legs if l.lifetime_ms is not None]
    connects = [l.connect_ms for l in legs if l.connect_ms is not None]
    return_zero = sum(1 for l in legs if l.return_bytes == 0 or l.bridge_to_front_bytes == 0)
    have_return_info = sum(
        1 for l in legs if l.return_bytes is not None or l.bridge_to_front_bytes is not None
    )

    m.n = len(legs)
    m.n_timeout = sum(1 for l in legs if l.is_timeout)
    m.timeout_rate = m.n_timeout / m.n if m.n else 0.0
    m.first_byte_p50 = pct(fb_vals, 0.5)
    m.first_byte_p90 = pct(fb_vals, 0.9)
    m.first_byte_max = max(fb_vals) if fb_vals else 0
    m.lifetime_p50 = pct(lifetimes, 0.5)
    m.lifetime_p90 = pct(lifetimes, 0.9)
    m.return_zero_rate = (return_zero / have_return_info) if have_return_info else 0.0
    m.connect_p50 = pct(connects, 0.5) if connects else None
    for l in legs:
        if l.reason:
            m.reason_dist[l.reason] = m.reason_dist.get(l.reason, 0) + 1
    return m


def gap_classification(hop_flows: Dict[str, HopFlows]) -> GapClass:
    """回程 gap 早段(我们瓶颈) vs 中后段(上游节奏)分类，跨 ingress+front 汇总。"""
    gc = GapClass()
    for hop in ("ingress", "front"):
        hf = hop_flows.get(hop)
        if hf is None:
            continue
        for leg in hf.legs:
            for g, ch in zip(leg.gaps, leg.gap_chunks):
                if g < GAP_BIG_MS:
                    continue
                gc.total_big += 1
                sample = f"  [{hop}] chunks={ch} gap={g}ms {leg.target}"
                if ch <= 2:
                    gc.early_stall += 1
                    if len(gc.samples_early) < 6:
                        gc.samples_early.append(sample)
                else:
                    gc.late_pacing += 1
                    if len(gc.samples_late) < 6:
                        gc.samples_late.append(sample)
    return gc


@dataclass
class HopAttribution:
    """双向逐跳归因：每跳 first_byte p50（回程首字节到达该跳的累计耗时）。"""

    egress_upstream_p50: int = 0  # egress 上游(KZ→TikTok)纯往返 first_byte
    egress_connect_p50: Optional[int] = None
    relay_p50: int = 0
    ingress_p50: int = 0
    front_p50: int = 0


def hop_attribution(hop_flows: Dict[str, HopFlows], target: Optional[str] = None) -> HopAttribution:
    """回程逐跳 first_byte 拆解：egress(上游) → relay → ingress → front。

    各跳 first_byte_ms 是「该跳从自身 open 到收到回程首字节」的耗时。
    egress 最贴近物理上游 RTT；逐跳递增的差额 = 该段隧道+处理叠加的协议开销。
    """
    a = HopAttribution()
    eg = hop_metrics(hop_flows["egress"], target)
    rl = hop_metrics(hop_flows["relay"], target)
    ing = hop_metrics(hop_flows["ingress"], target)
    fr = hop_metrics(hop_flows["front"], target)
    a.egress_upstream_p50 = eg.first_byte_p50
    a.egress_connect_p50 = eg.connect_p50
    a.relay_p50 = rl.first_byte_p50
    a.ingress_p50 = ing.first_byte_p50
    a.front_p50 = fr.first_byte_p50
    return a


def is_healthy(front_metrics: HopMetrics) -> bool:
    """健康基线：前端回程 first_byte p50 < 660ms 且超时率低。"""
    return (
        front_metrics.first_byte_p50 > 0
        and front_metrics.first_byte_p50 < HEALTH_FIRST_BYTE_P50_MS
        and front_metrics.timeout_rate < 0.05
    )
