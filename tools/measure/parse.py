"""解析层：把各跳原始日志行解析成统一的 TraceEvent。

格式分流（来自源码确证）：
  - front（xgw-edge-server, Go）：每行是 JSON，业务内容在 "msg" 字段（key=value 串）。
    关键行：tiktok.trace.front.{open,first_packet,first_byte,close}、
            hy2front.flow.chunk ... direction=bridge_to_front、hy2front.flow.accounting。
  - C 三跳（ingress/relay/egress）：纯文本 key=value，空格分隔。
    关键行：tiktok.trace.{ingress,relay,egress}.{accept,first_packet,first_byte,close}、
            ingress.phase.return_chunk、egress.phase.{open,close}、cc.stall（P0 新增）。

关联键：同跳内用 stream(C)/conn(front) + target 收拢一条流。
跨跳禁止按 stream join —— stream 号各进程独立、跨跳不相等。
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# key=value：value 可为 -?数字、host:port、或非空白串。
_KV_RE = re.compile(r"(\w+)=(-?\d+\.?\d*|\S+)")


@dataclass
class TraceEvent:
    hop: str  # ingress / relay / egress / front
    kind: str  # accept / first_packet / first_byte / close / return_chunk / chunk / open / cc_stall ...
    target: str  # 归一化 host:port
    line_no: int  # 在该跳日志内的行号（时间窗关联用）
    fields: Dict[str, str] = field(default_factory=dict)

    def num(self, key: str, default: int = 0) -> int:
        v = self.fields.get(key)
        if v is None:
            return default
        try:
            return int(float(v))
        except (TypeError, ValueError):
            return default

    def local_id(self) -> Optional[int]:
        """同跳内的本地流号：C 层 stream，front 层 conn。"""
        for k in ("stream", "conn"):
            if k in self.fields:
                try:
                    return int(self.fields[k])
                except ValueError:
                    return None
        return None


def normalize_target(raw: str) -> str:
    """统一 host:port 形式，去掉可能的引号/尾随逗号。"""
    if not raw:
        return ""
    return raw.strip().strip('"').rstrip(",")


def _kv(s: str) -> Dict[str, str]:
    return {m.group(1): m.group(2) for m in _KV_RE.finditer(s)}


# 各跳「跳前缀.kind」识别：msg/文本里的 token -> (hop, kind)
# front 用 tiktok.trace.front.* 与 hy2front.flow.*；C 用 *.phase.* 与 tiktok.trace.*。
_FRONT_PREFIXES = {
    "tiktok.trace.front.open": "open",
    "tiktok.trace.front.first_packet": "first_packet",
    "tiktok.trace.front.first_byte": "first_byte",
    "tiktok.trace.front.close": "close",
}


def parse_front_line(line: str, line_no: int) -> Optional[TraceEvent]:
    """front 行：先 json.loads 取 msg，再按 token 分类。"""
    line = line.strip()
    if not line.startswith("{"):
        return None
    try:
        obj = json.loads(line)
    except (json.JSONDecodeError, ValueError):
        return None
    msg = obj.get("msg", "")
    if not msg:
        return None

    kind = None
    for tok, k in _FRONT_PREFIXES.items():
        if msg.startswith(tok):
            kind = k
            break
    # 回程 chunk（含 accounting 收支）只关心 bridge_to_front 方向。
    if kind is None:
        if msg.startswith("hy2front.flow.chunk") and "direction=bridge_to_front" in msg:
            kind = "chunk_back"
        elif msg.startswith("hy2front.flow.accounting"):
            kind = "accounting"
        else:
            return None

    fields = _kv(msg)
    target = normalize_target(fields.get("target", ""))
    if kind in ("open", "first_packet", "first_byte", "close") and "tiktok" not in target:
        # tiktok.trace.front.* 已自带 tiktok target，但 chunk/accounting 来自所有流，需过滤。
        pass
    return TraceEvent(hop="front", kind=kind, target=target, line_no=line_no, fields=fields)


# C 层文本行 token -> (hop, kind)
_C_TOKENS = {
    "tiktok.trace.ingress.accept": ("ingress", "accept"),
    "tiktok.trace.ingress.first_packet": ("ingress", "first_packet"),
    "tiktok.trace.ingress.first_byte": ("ingress", "first_byte"),
    "tiktok.trace.ingress.close": ("ingress", "close"),
    "ingress.phase.return_chunk": ("ingress", "return_chunk"),
    "tiktok.trace.relay.first_packet": ("relay", "first_packet"),
    "tiktok.trace.relay.first_byte": ("relay", "first_byte"),
    "tiktok.trace.relay.close": ("relay", "close"),
    "relay.phase.close": ("relay", "close_all"),
    "tiktok.trace.egress.first_byte": ("egress", "first_byte"),
    "tiktok.trace.egress.close": ("egress", "close"),
    "egress.phase.open": ("egress", "open"),
    "egress.phase.close": ("egress", "close_all"),
    "cc.stall": (None, "cc_stall"),  # hop 由日志来源决定
}


def parse_c_line(line: str, hop_source: str, line_no: int) -> Optional[TraceEvent]:
    """C 层纯文本行。hop_source 是日志来源（ingress/relay/egress）。"""
    line = line.strip()
    if not line or line.startswith("{"):
        return None
    token = line.split(" ", 1)[0]
    spec = _C_TOKENS.get(token)
    if spec is None:
        return None
    hop, kind = spec
    if hop is None:
        hop = hop_source  # cc.stall 等通用行，归属来源跳
    fields = _kv(line)
    target = normalize_target(fields.get("target", ""))
    return TraceEvent(hop=hop, kind=kind, target=target, line_no=line_no, fields=fields)


def parse_bundle(bundle) -> List[TraceEvent]:
    """解析整个 RawBundle -> TraceEvent 列表（front + 三跳 C 层）。"""
    events: List[TraceEvent] = []
    # 前端。
    front_raw = bundle.read("front")
    for i, line in enumerate(front_raw.splitlines()):
        ev = parse_front_line(line, i)
        if ev is not None:
            events.append(ev)
    # C 三跳。
    for hop in ("ingress", "relay", "egress"):
        raw = bundle.read(hop)
        for i, line in enumerate(raw.splitlines()):
            ev = parse_c_line(line, hop, i)
            if ev is not None:
                events.append(ev)
    return events
