# NB (Newbility) 需求文档

> 文档集导航：01-需求(本篇) | [02-架构](02-architecture.md) | [03-已实现](03-implementation.md) | [04-部署运维](04-deployment-ops.md) | [05-路线图](05-roadmap.md) | [06-自研客户端](06-custom-client.md) | [fec-plan](fec-plan.md)

## 1. 项目定位

**NB (Newbility)** 是自研的**可级联 TCP-over-QUIC 多跳隧道**，用于把手机流量（主要 TikTok）经多跳中转到目标，规避网络限制、优化跨境传输质量。基于 picoquic (BBRv3) 传输内核。

前身：`E:\project\wireguard-linux\tools\xgw`（自研协议 hy2-front → ingress → relay → egress）。xgw 因**乘性放大雪崩**（每跳单线程调度/pacing自旋/recv轮询/FEC/加密开销被 TLS 握手往返×跳数放大 6–12×）导致 TikTok "无网络连接"。结论：不再打补丁，改用成熟 QUIC 内核(picoquic)，保留自研 BBRv2 拥塞控制与规划中的 FEC，角色名(entry/middle/exit)保留。

## 2. 核心诉求

在**原有机器**上跑手机 TikTok（Shadowrocket SOCKS5 客户端），达到：

| 指标 | 要求 |
|---|---|
| **稳定** | 无雪崩、无泄漏、长时间运行不退化、故障自愈 |
| **高并发** | TikTok 打开瞬间几十~上百并发流不卡、不排队 |
| **大流量** | 常态 5Mbps，**偶发峰值 80Mbps**（视频预加载） |
| **直播不卡顿** | 直播（webcast/RTC）低延迟、抗抖动，连接质量"绿色" |

对标：优于纯 hy2。hy2 延迟不如 NB，但 hy2 直播稳定性此前优于 NB（因 hy2 带 FEC/brutal）。

## 3. 使用场景

- **主场景**：手机 App(TikTok) → Shadowrocket(SOCKS5) → NB 三跳隧道 → 目标。观看视频、直播、拍卖等实时 TCP 流。
- **业务差异**：直播/拍卖是**延迟敏感实时流**；视频预加载是**大流量批量流**；API 是小请求。三类需差异化处理（分流/优先级）。

## 4. 产品方向（决定架构取向）

**卖线路**：起点、终点地区确定，中间路径可动态优化/探测选最优/跳数可变。因此：
- **中间节点无状态**：路径完全由 entry 下发的 source-route 决定（`H:hop:port,...,T:target:port`），适配"卖线路 + 动态选路 + 路径探测"。
- 控制平面（规划）：探测 + 加权最短路选路 + source-route 下发。
- 需要**用户认证鉴权**（卖线路的前提，当前缺失）。
- 需要**白名单/分流**：只代理约定流量（TikTok），防止用户其他流量占用线路带宽 + 降低安全风险。

## 5. 功能需求清单

- [x] 多跳 TCP-over-QUIC 隧道（entry/middle/exit 三角色，一份二进制）
- [x] source-route 动态选路地基
- [x] SOCKS5 入口（手机接入）
- [x] 高并发流管理（无泄漏、无雪崩）
- [x] 大流量吞吐（窗口/buffer/批量收发）
- [x] 流优先级/直播专用道
- [x] 白名单访问控制（IP/域名/端口 + 远程热配）
- [x] 链路质量观测（丢包/RTT）
- [~] **FEC 前向纠错**（抗抖动，直播稳定性最后一环）— 已完成直播延迟流双发/去重版本，待量化收益与自适应
- [ ] 用户认证鉴权（卖线路前提）
- [ ] **限速能力（QoS/套餐限速）**：用户套餐限速→NB应用层token bucket(依赖认证,QUIC加密XDP看不到用户)；节点/IP级限速防护→复用xgw XDP。详见 05-roadmap。
- [ ] 控制平面（探测+选路+下发）
- [ ] 自研客户端（替代 Shadowrocket，见 06）
- [ ] 多线程/多进程规模化（多用户）

## 6. 非功能需求

- **平台**：Linux x86_64（三跳服务器）；客户端目前 iOS/Android via Shadowrocket。
- **可运维**：远程配置热重载（白名单已实现）、日志可观测、一键部署。
- **可扩展**：为多用户规模化预留（多线程 SO_REUSEPORT）。
