"""关联层：把跨跳 TraceEvent 聚类成 FlowView。

纪律（来自架构约束）：
  - stream 号各进程独立、跨跳不相等 —— 禁止按 stream 跨跳 join。
  - 关联主键 = 归一化 target；同跳内用 local_id(stream/conn) 收拢一条流；
    跨跳用 target 聚合 + 因果顺序（egress→relay→ingress→front 的 first_byte 时间单调）。
  - 不构造全局唯一流号（脆弱拼接风险高）。FlowView 既支持对齐后的端到端视图，
    也允许退化为各跳独立分布（与现有可靠判读一致）。
"""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from .parse import TraceEvent


@dataclass
class FlowLeg:
    """一条流在「某一跳」的视图（同跳内按 local_id 收拢）。"""

    hop: str
    local_id: Optional[int]
    target: str
    first_packet_ms: Optional[int] = None  # 去程：收到客户端/上游请求
    first_byte_ms: Optional[int] = None  # 回程：上游响应首字节到本跳
    lifetime_ms: Optional[int] = None
    connect_ms: Optional[int] = None  # egress 上游连接耗时
    return_bytes: Optional[int] = None  # C 层回程字节
    bridge_to_front_bytes: Optional[int] = None  # 前端回程字节（accounting）
    reason: Optional[str] = None
    is_timeout: bool = False  # first_byte 为 -1
    gaps: List[int] = field(default_factory=list)  # 回程 chunk gap_ms（>=0）
    gap_chunks: List[int] = field(default_factory=list)  # 各 gap 对应 chunks 计数


@dataclass
class HopFlows:
    """某一跳所有流的聚合（per-hop 独立视图，最可靠）。"""

    hop: str
    legs: List[FlowLeg]


def _legs_for_hop(events: List[TraceEvent], hop: str) -> List[FlowLeg]:
    """同跳内按 (local_id, target) 收拢成 FlowLeg。"""
    by_key: Dict[tuple, FlowLeg] = {}

    def leg_for(ev: TraceEvent) -> FlowLeg:
        key = (ev.local_id(), ev.target)
        leg = by_key.get(key)
        if leg is None:
            leg = FlowLeg(hop=hop, local_id=ev.local_id(), target=ev.target)
            by_key[key] = leg
        return leg

    for ev in events:
        if ev.hop != hop:
            continue
        leg = leg_for(ev)
        if ev.kind in ("first_packet",):
            leg.first_packet_ms = ev.num("elapsed_ms", leg.first_packet_ms or 0)
        elif ev.kind in ("first_byte",):
            leg.first_byte_ms = ev.num("elapsed_ms", leg.first_byte_ms or 0)
        elif ev.kind == "open":
            # egress.phase.open: connect_ms
            if "connect_ms" in ev.fields:
                leg.connect_ms = ev.num("connect_ms")
        elif ev.kind in ("close", "close_all"):
            leg.lifetime_ms = ev.num("lifetime_ms", leg.lifetime_ms or 0)
            fb = ev.num("first_byte_ms", -999)
            if fb == -1:
                leg.is_timeout = True
            elif fb >= 0 and leg.first_byte_ms is None:
                leg.first_byte_ms = fb
            if "return_bytes" in ev.fields:
                leg.return_bytes = ev.num("return_bytes")
            if "recv_bytes" in ev.fields:  # egress 视角的回程字节
                leg.return_bytes = ev.num("recv_bytes")
            if "reason" in ev.fields:
                leg.reason = ev.fields.get("reason")
        elif ev.kind == "return_chunk":
            g = ev.num("gap_ms", -1)
            if g >= 0:
                leg.gaps.append(g)
                leg.gap_chunks.append(ev.num("chunks", 0))
        elif ev.kind == "chunk_back":  # 前端 bridge_to_front
            g = ev.num("gap_ms", -1)
            if g >= 0:
                leg.gaps.append(g)
                leg.gap_chunks.append(ev.num("chunks", 0))
        elif ev.kind == "accounting":
            leg.bridge_to_front_bytes = ev.num("bridge_to_front_bytes")
    return list(by_key.values())


def build_hop_flows(events: List[TraceEvent]) -> Dict[str, HopFlows]:
    """构建 per-hop 独立视图（front/ingress/relay/egress）。这是最可靠的基础。"""
    out: Dict[str, HopFlows] = {}
    for hop in ("front", "ingress", "relay", "egress"):
        out[hop] = HopFlows(hop=hop, legs=_legs_for_hop(events, hop))
    return out


def legs_by_target(hop_flows: HopFlows) -> Dict[str, List[FlowLeg]]:
    """某跳内按 target 分组（per-target 指标用）。"""
    by_t: Dict[str, List[FlowLeg]] = defaultdict(list)
    for leg in hop_flows.legs:
        by_t[leg.target].append(leg)
    return dict(by_t)
