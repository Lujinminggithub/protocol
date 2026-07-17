# 03 异步裁决机制

> 上级：[README](./README.md) ｜ 相关：[01 总体架构](./01-architecture-overview.md)、[05 内容检测与策略](./05-content-and-policy.md)
> **本机制是 DLP 深度内容检测的架构前提**（ADR D4）。

## 为什么需要

真 DLP 要看内容再决定放行/阻断（这份外发数据是否含敏感信息）。内容检测（正则/指纹/OCR/ML）是重逻辑，**不能放内核**。因此内核 classify 不能同步等结果，必须：**抓内容 → 挂起 IO → 送用户态检测 → 回裁决 → 完成 IO**。

当前是内核本地同步判定，做不了深度检测——这是最大的结构性缺口。

## 现状

- 文件侧 `PreCreate/PreWrite/PreSetInformation` 同步返回 allow/block（`policy_engine` 本地判定）。
- 网络侧 STREAM/ALE classify 同步返回，无 `FwpsPendClassify0`。

## 目标数据流

```
① Provider 命中「需深度检测」(轻判定无法定论)
        │
② 抓内容样本(头部/body片段/文件片段) → 生成裁决请求(reqId)
        │
③ PEND 挂起 IO：
     网络: FwpsPendClassify0 保存 classifyHandle
     文件: 返回 FLT_PREOP_PENDING, 保留 CallbackData
        │
④ 请求入「裁决队列」→ 经 FltPort/IOCTL 送用户态
        │
⑤ 用户态: 内容检测引擎 + 策略决策 → 裁决(allow/block/加密/脱敏, reqId)
        │
⑥ 裁决回写内核 → 按 reqId 找回挂起上下文
        │
⑦ COMPLETE 挂起 IO：
     网络: FwpsCompleteClassify0 + 释放 handle
     文件: FltCompletePendedPreOperation
        │
⑧ 超时兜底: T 毫秒无回裁决 → 按 fail 策略放行/拦截 + 记录
```

## 快路径 vs 慢路径（性能红线）

绝大多数 IO **不 pend**，走内核轻判定快路径直接放行；只有「疑似敏感」少量 IO 才 pend 走用户态。判定是否 pend 的依据（内核侧廉价条件）：

- 目标通道敏感（外发网络 / 可移动卷写入）
- 命中轻规则但需内容确认（如敏感扩展名 + 大小阈值）
- 判定缓存未命中（见下）

> 若每个 IO 都 pend，跨态往返会拖垮吞吐。**pend 是例外，不是常态。**

## 裁决缓存（避免重复跨态）

用户态裁决结果回填内核 `FastVerdictCache`：
- key = hash(策略版本 ‖ 主体 ‖ 通道 ‖ 目标特征)
- 命中且未过期 → 内核直接复用，不再 pend
- 失效：策略版本号(`PolicyGeneration`)自增即全体惰性失效

## 关键约束参数（写进策略）

| 参数 | 说明 | 建议 |
|---|---|---|
| 裁决超时 T | 用户态无响应的等待上限 | 按通道分级，如网络 200ms、文件 500ms |
| fail 策略 | 超时/引擎不可用时的默认动作 | **按数据分类**：机密 fail-close，普通 fail-open |
| 最大在途请求数 | 挂起 IO 上限，防积压耗尽资源 | 有上限，超限降级为快路径 fail 策略 |
| 引擎健康 | 用户态引擎存活检测 | 掉线 → 全部走 fail 策略 + 告警 |

## 正确性要点

- 挂起上下文的生命周期：pend 时持有的引用（classifyHandle / CallbackData / NBL）必须在 complete 或超时时精确释放，避免泄漏/UAF。
- 卸载/引擎退出：需把所有在途挂起 IO 按 fail 策略完成，不能留悬挂。
- 网络 `FwpsPendClassify0` 要求正确管理 flow 引用；配合注册 flowDeleteFn（见 [04](./04-data-channels.md)）。
- reqId 防重放/防错配：一次性、带校验。

## 待办

- [ ] Core 新增「裁决队列 + 挂起上下文表 + 超时定时器」。
- [ ] 网络侧接入 `FwpsPendClassify0/FwpsCompleteClassify0`。
- [ ] 文件侧接入 minifilter pending + `FltCompletePendedPreOperation`。
- [ ] `FastVerdictCache` + `PolicyGeneration` 版本失效。
- [ ] 定义 fail 策略与超时的策略字段。

## 涉及文件（规划）

- Core：`src/kernel/core/adjudicate.*`（新增）
- 网络：`src/kernel/network/`（classify 改造）
- 文件：`src/kernel/filter/`（pending 改造）
- 用户态：`src/native/src/dlp/`（裁决回路 + 引擎对接）
