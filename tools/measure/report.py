"""判读层：瓶颈定位 + 金标准并列对比 + 文本报告。

瓶颈判读逻辑（来自诊断结论）：
  - egress 上游 first_byte 小，但 ingress/front 大 => 隧道回程慢（我们的问题）。
  - return_bytes>0 但 front bridge_to_front_bytes==0 => 桥接出队/前端写回断点。
  - 回程 gap 大头是 early_stall => 回程首段滞留（我们瓶颈）；late_pacing => 上游节奏（非我们）。
  - timeout_rate 高且 lifetime~固定上限 => 真超时；否则竞速被客户端杀。
"""

from __future__ import annotations

from typing import Dict, List, Optional

from .correlate import HopFlows
from .metrics import (
    GapClass,
    HopMetrics,
    gap_classification,
    hop_attribution,
    hop_metrics,
    is_healthy,
)


def bottleneck_verdict(hop_flows: Dict[str, HopFlows], target: Optional[str] = None) -> List[str]:
    out: List[str] = []
    attr = hop_attribution(hop_flows, target)
    fr = hop_metrics(hop_flows["front"], target)
    gc = gap_classification(hop_flows)

    out.append("=== 双向逐跳归因（回程 first_byte p50，单位 ms）===")
    out.append(
        f"  egress上游(纯往返)={attr.egress_upstream_p50}"
        + (f"  egress连接={attr.egress_connect_p50}" if attr.egress_connect_p50 is not None else "")
    )
    out.append(f"  relay={attr.relay_p50}  ingress={attr.ingress_p50}  front={attr.front_p50}")

    # 逐段增量归因。
    if attr.egress_upstream_p50 > 0:
        eg_to_front = attr.front_p50 - attr.egress_upstream_p50
        out.append(
            f"  => egress→front 隧道+处理叠加 ≈ {eg_to_front}ms"
            f"（egress 上游 {attr.egress_upstream_p50}ms 是物理基线）"
        )
        if attr.egress_upstream_p50 < 400 and attr.front_p50 >= 1000:
            out.append("  ★ 判读：上游往返正常但前端首字节被拖到 ~1s+ => 隧道回程是瓶颈（我们的问题）")
        elif attr.egress_upstream_p50 >= 600:
            out.append("  ★ 判读：egress 上游本身就慢 => 瓶颈在 KZ→上游，非隧道")

    # 桥接断点检测。
    fr_zero = fr.return_zero_rate
    if fr_zero > 0.1:
        out.append(f"  ★ 前端回程零字节流占比 {fr_zero:.0%} => 疑似桥接出队/前端写回断点")

    out.append("")
    out.append(f"=== 回程 gap 位置（gap>=1s 共 {gc.total_big} 个）===")
    out.append(f"  流开头卡(chunks<=2, 我们的瓶颈): {gc.early_stall}")
    for s in gc.samples_early:
        out.append(s)
    out.append(f"  流中后段(chunks>3, 上游节奏): {gc.late_pacing}")
    for s in gc.samples_late:
        out.append(s)
    if gc.total_big:
        if gc.early_stall > gc.late_pacing:
            out.append("  ★ 判读：回程首段就卡 => 是我们的问题，继续定位接收侧滞留 / cc_stall")
        else:
            out.append("  ★ 判读：大头是上游节奏 => first_byte 才是关键指标")

    out.append("")
    out.append(f"=== 健康基线 ===")
    out.append(
        f"  前端 first_byte p50={fr.first_byte_p50}ms 超时率={fr.timeout_rate:.0%} "
        f"=> {'健康(<660ms)' if is_healthy(fr) else '不健康(需 P1 修复)'}"
    )
    return out


def _fmt_hop(m: HopMetrics) -> str:
    return (
        f"{m.hop:>7}: n={m.n:<4} 超时率={m.timeout_rate:.0%} "
        f"first_byte p50={m.first_byte_p50} p90={m.first_byte_p90} max={m.first_byte_max} "
        f"lifetime p50={m.lifetime_p50}"
    )


def render_report(hop_flows: Dict[str, HopFlows], target: Optional[str] = None) -> str:
    lines: List[str] = []
    scope = f"（target~{target}）" if target else "（全部 TikTok 流）"
    lines.append(f"######## xgw 三跳测量报告 {scope} ########")
    lines.append("")
    lines.append("--- per-hop 指标 ---")
    for hop in ("egress", "relay", "ingress", "front"):
        lines.append(_fmt_hop(hop_metrics(hop_flows[hop], target)))
    lines.append("")
    lines.extend(bottleneck_verdict(hop_flows, target))
    return "\n".join(lines)


def render_compare(xgw_front: HopMetrics, gold) -> str:
    """金标准（纯 hy2）vs xgw 并列对比。gold 为 singbox.GoldMetrics 列表。"""
    lines: List[str] = []
    lines.append("######## 金标准对比：纯 hy2 vs xgw ########")
    lines.append(f"  xgw   前端 first_byte p50={xgw_front.first_byte_p50}ms 超时率={xgw_front.timeout_rate:.0%}")
    if gold:
        for g in gold:
            lines.append(
                f"  hy2   {g.target}: first_byte p50={g.first_byte_p50}ms "
                f"成功率={g.success_rate:.0%}  {g.note}"
            )
    else:
        lines.append("  hy2   （无金标准数据，需 compare --probe 主动探测）")
    lines.append("")
    lines.append("  注：sing-box log.level=info 不输出 per-flow first_byte，")
    lines.append("      金标准须用主动同口径探测（客户端侧测），不能依赖被动日志。")
    return "\n".join(lines)
