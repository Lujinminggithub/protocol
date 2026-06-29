"""P0 TCP 回程质量统一测量框架。

设计原则（不可违反）：
  1. 纯只读：绝不重启 C 层/前端，绝不前台跑 xgw（抢 51840 端口 + shm-direct ring
     会致 ingress 反复崩溃——惨痛教训）。所有远端命令仅 tail / cat /proc/*/fd/1 / pgrep。
  2. 采集与解析分离：collect 先把原始日志 dump 到本地 tmp/measure/<run_id>/，
     之后所有解析/指标/对比都离线跑同一份样本，保证可复现（同一 run 多次解析结果一致）。
  3. 统一口径：所有 first_byte / 回程 gap / 超时率的判读标准收敛到本包，
     取代 tools/diag_*.py 里各自为政的一次性 tail+grep。

模块：
  collect    采集层：只读拉三跳 C 层 + 前端日志到本地。
  parse      解析层：front 行 JSON / C 行纯文本分流，统一成 TraceEvent。
  correlate  关联层：按 target + 时间窗聚类成 FlowView（禁止按 stream 跨跳 join）。
  metrics    指标层：双向逐跳归因、p50/p90/max、超时率、回程 gap 分类。
  report     判读层：瓶颈定位 + 金标准并列对比。
  singbox    金标准适配（主动同口径探测）。
  cli        命令行入口：collect / analyze / compare / live。
"""

from __future__ import annotations

__all__ = [
    "collect",
    "parse",
    "correlate",
    "metrics",
    "report",
]
