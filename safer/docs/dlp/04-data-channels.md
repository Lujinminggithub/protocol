# 04 数据通道 Provider

> 上级：[README](./README.md) ｜ 相关：[03 异步裁决](./03-async-adjudication.md)、[05 内容检测与策略](./05-content-and-policy.md)

## 核心结论：通道 provider 横跨内核与用户态

不是所有外泄通道都能在内核实现。通道抽象为「provider」，接入统一 Core（策略/内容检测/上报），但**分布在内核与用户态两侧**：

| 通道 | 采集/拦截层 | 手段 | 现状 |
|---|---|---|---|
| 文件 | 内核 | MiniFilter（CREATE/WRITE/SET_INFO）| ✅ 已有 |
| 网络 | 内核 | WFP（ALE + STREAM）| ✅ 已有 |
| **USB/可移动介质** | 内核 | ①设备控制(PnP/卷过滤) ②内容感知(识别目标卷 `DRIVE_REMOVABLE`) | ❌ 待建 |
| **打印** | 用户态为主 | Print Processor / Port Monitor / spooler 监控 | ❌ 待建 |
| **剪贴板** | 仅用户态 | 剪贴板监听（内核不可）| ❌ 待建 |
| **截屏** | 仅用户态 | 用户态 hook（内核不可）| ❌ 待建 |

> 因此 Core 的策略/检测/上报接口必须**同时服务内核 provider 与用户态 provider**。用户态 Agent 既是深度检测引擎，也是剪贴板/截屏/打印的采集点。

## Provider 统一契约（无论内核/用户态）

每个 provider 对 Core 暴露一致的交互：
1. **采集**：捕获操作事件 + 内容样本（进程/用户/目标/内容片段）。
2. **轻判定**（可选）：本地快规则命中即直接放行/拦截。
3. **裁决请求**：需深度检测时提交裁决请求（内核走 pend，见 [03](./03-async-adjudication.md)；用户态直接调引擎）。
4. **执行动作**：allow/block/加密/脱敏/告警。
5. **上报**：事件写入可靠管道（见 [07](./07-comm-and-audit.md)）。

## 各通道要点

### 文件（内核，已有）
- 现状：观测 + 阻断 + 隔离重定向；已注册 CREATE/WRITE/SET_INFO。
- 目标补强：READ/CLEANUP 回调（支撑读重定向/透明加密）；写入前抓内容送裁决。

### 网络（内核，已有）
- 现状：观测 + 阻断；ALE + STREAM。
- 目标补强：STREAM body 抓样送裁决（pend）；可选 CONNECT_REDIRECT（重定向到本地代理，见旧网络重定向分析）。

### USB/可移动介质（内核，新建）——DLP 头号场景
两条腿：
- **设备控制**：按设备类型/实例白名单，允许或禁用整个可移动设备（PnP 控制或卷过滤）。
- **内容感知**：允许设备但拦截「敏感文件写入可移动卷」。**可复用现有 MiniFilter**——在 `PreCreate/PreWrite` 中识别目标卷为 `DRIVE_REMOVABLE`（或按卷设备类型判断），命中则走内容裁决。
- 建议优先「内容感知」（复用面大），设备控制作为强策略补充。

### 打印（用户态为主，新建）
- 手段：spooler 侧监控（Print Processor / Port Monitor），或用户态挂打印 API。
- 采集打印作业内容 → 送检测引擎 → 允许/阻断/加水印。

### 剪贴板（用户态，新建）
- 监听剪贴板变化，命中敏感内容时清空/拦截/告警。

### 截屏（用户态，新建）
- 用户态 hook 截屏路径；对含敏感窗口的截屏拦截/打码/告警。

## 待办

- [ ] 定义 Core 的 provider 接口（采集/轻判定/裁决/执行/上报五段）。
- [ ] USB：卷类型识别 + 可移动卷写入内容裁决（复用 MiniFilter）；设备控制策略。
- [ ] 打印：spooler 监控 provider（用户态）。
- [ ] 剪贴板/截屏：用户态采集 provider。

## 涉及文件（规划）

- 内核：`src/kernel/filter/`（USB 卷识别复用）、`src/kernel/core/provider.*`（接口）
- 用户态：`src/native/src/dlp/channels/`（打印/剪贴板/截屏）
