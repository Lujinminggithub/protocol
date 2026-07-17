# PersonalSafer DLP 架构文档（主索引）

> 本目录是 PersonalSafer 面向 **DLP（数据防泄漏）方向** 的架构文档集。
> 采用「主索引 + 专题拆分」结构，每篇聚焦一个主题、篇幅可控、便于独立修改。
> 顶层旧文档见 [`../architecture.md`](../architecture.md)、[`../kernel-design.md`](../kernel-design.md)，本目录是其在 DLP 方向上的细化与演进。

## 文档导航

| 编号 | 文档 | 内容 |
|---|---|---|
| — | [README.md](./README.md) | 本索引：导航 + 架构决策摘要 + 路线索引 |
| 01 | [01-architecture-overview.md](./01-architecture-overview.md) | 总体架构、DLP 能力地图、分层主图、内核/用户态职责边界 |
| 02 | [02-kernel-architecture.md](./02-kernel-architecture.md) | 单 sys 内部分层（Core + Providers）、INF、加载/卸载编排、BFE 依赖 |
| 03 | [03-async-adjudication.md](./03-async-adjudication.md) | 异步裁决机制（内核 pend → 用户态检测 → 回裁决），DLP 深度检测的前提 |
| 04 | [04-data-channels.md](./04-data-channels.md) | 数据通道 provider：文件/网络/USB/打印/剪贴板/截屏（内核 vs 用户态分布）|
| 05 | [05-content-and-policy.md](./05-content-and-policy.md) | 内容检测引擎（用户态）+ 策略模型（分类×主体×通道×动作矩阵）|
| 06 | [06-self-protection.md](./06-self-protection.md) | 自我保护 + 统一解锁/卸载（认证、状态机、卸载路线）|
| 07 | [07-comm-and-audit.md](./07-comm-and-audit.md) | 通信通道、事件管道、可靠上报、取证、预留中心化接口 |
| 08 | [08-roadmap.md](./08-roadmap.md) | 演进路线图（分阶段落地）|

## 架构决策摘要（ADR 速查）

已在讨论中拍板的关键决策，详情见对应文档：

| # | 决策 | 取值 | 依据 |
|---|---|---|---|
| D1 | 驱动形态 | **单一 sys + 内部分层**（Core + Providers） | 文件/网络/进程需强关联、共享策略与通信，内核态拆分买不到崩溃隔离 |
| D2 | INF/服务注册 | **单一 minifilter 服务**；WFP callout 运行时 API 注册（不进 INF） | 一个 sys=一个内核服务；WFP 靠 `FwpsCalloutRegister0` 动态注册 |
| D3 | 产品形态 | **端侧为主，预留 Agent-Server 分层接口**（暂不实现 Server） | 控制范围，预留而非重构 |
| D4 | 内容检测 | **用户态检测引擎 + 内核异步裁决（pend）** | 重内容检测（正则/指纹/OCR/ML）不能放内核 DISPATCH 层 |
| D5 | 通道扩展 | 文件/网络之外新增 **USB/可移动介质、打印、剪贴板/截屏** | 覆盖主要外泄出口；通道 provider 横跨内核/用户态 |
| D6 | 自保护认证 | **挑战-响应 + 非对称验签 + 调用方自家签名进程校验（A+B 双因子）** | 防伪造 IOCTL + 防滥用 |
| D7 | 自保护范围 | **文件/目录 + MiniFilter 防卸载 + 进程防杀** | 进程/注册表保护需 `/INTEGRITYCHECK` 有效签名 |
| D8 | 解锁粒度 | **全局解锁 + 短 TTL（默认 5 分钟）+ 主动 RELOCK** | 适配卸载场景，简单可控 |

## 全局架构约束（写代码时的红线）

1. **内核只抓不判深**：重内容检测一律用户态；内核只做拦截点 + 内容抓取 + 轻量判定 + pend。
2. **通道 provider 化**：新增通道只依赖 Core 接口，不改 Core。
3. **审计不丢**：事件管道「可靠」优先于「低延迟」；满队列不得静默丢弃。
4. **自保护默认 LOCKED**：驱动加载即锁定（fail-safe），解锁通道在任何状态下必须可达。
5. **fail 策略显式化**：异步裁决超时按「通道/数据分类」决定 fail-open 或 fail-close。

## 现状基线（文档撰写时的代码事实）

- 内核：单 `PersonalSafer.sys`，含 MiniFilter（`filter/`）+ WFP（`network/`）+ 策略（`policy/`）+ 通信（`device.c`）。
- 文件侧：已实现「观测 + 阻断 + 隔离重定向」；已注册 `IRP_MJ_CREATE/WRITE/SET_INFORMATION`（无 READ/CLEANUP/Post）。
- 网络侧：已实现「观测 + 阻断」；注册 ALE_AUTH_CONNECT/RECV_ACCEPT + STREAM；**无重定向/注入/pend**。
- 策略：元数据匹配（扩展名/端口/域名/URL/HTTP/FTP），线性遍历，无判定缓存，**无真正内容检测**。
- 通信：IOCTL 单条轮询上报；环形缓冲满即丢、无丢弃计数。
- 尚缺：内容检测引擎、异步裁决、USB/打印/剪贴板通道、自保护、可靠上报、中心化。

> 说明：本文档描述**目标架构与演进方案**，与现状的差距即「待办」，各专题文档末尾列出。
