# 08 演进路线图

> 上级：[README](./README.md)
> 分阶段落地。每阶段可独立交付、独立验证。顺序遵循「先稳地基，再上能力」。

## 阶段 0：地基加固（稳定性/正确性，先做）
> 现有代码在高负载下会蓝屏/漏审计，新功能建立在不稳地基上无意义。

- [ ] `PreWrite` 补 `PAGED_CODE()` + 同步隔离写前断言 PASSIVE。
- [ ] 事件缓冲：丢弃计数 + 扩容/紧凑序列化（审计不丢）。
- [ ] 卸载路径引入 `IO_REMOVE_LOCK`；`FwpsCalloutUnregisterById0` 检查返回值。
- [ ] 网络 classify 的进程名解析移出 DISPATCH（IRQL 违规隐患）。
- [ ] 进程表墓碑延迟回收（晚到事件归属）。
- [ ] 修正 INF 三处问题（见 [02](./02-kernel-architecture.md)）。

## 阶段 1：自我保护（独立价值，优先级高）
> 详见 [06](./06-self-protection.md)。

- [ ] `protect` 状态机 + 5 个 IOCTL（桩认证跑通）。
- [ ] 文件保护（MiniFilter 前置）+ FilterUnload 防卸载。
- [ ] 进程防杀（`ObRegisterCallbacks`，确认签名前提）。
- [ ] A+B 认证 + 卸载器同一 unlock 路线。

## 阶段 2：Core 分层 + 异步裁决地基
> 详见 [02](./02-kernel-architecture.md)、[03](./03-async-adjudication.md)。

- [ ] 抽出 Core 接口（policy/process/event/adjudicate/protect），Provider 只依赖接口。
- [ ] 裁决队列 + 挂起上下文 + 超时 + fail 策略。
- [ ] 网络 `FwpsPendClassify`/文件 minifilter pending 接入。
- [ ] `FastVerdictCache` + `PolicyGeneration` 版本失效。

## 阶段 3：内容检测引擎（DLP 灵魂）
> 详见 [05](./05-content-and-policy.md)。

- [ ] 用户态检测引擎 L1-L3（真实类型 / 关键字字典 / 正则模板）。
- [ ] 数据分类模型 + 标签。
- [ ] 策略升级四维矩阵 + 身份维度（进程签名 / 用户 SID）。
- [ ] 内核轻判定性能改造（位图/Trie/AC/push lock）。
- [ ] 后续：L4-L6（指纹/OCR/ML）。

## 阶段 4：通道扩展
> 详见 [04](./04-data-channels.md)。

- [ ] USB/可移动介质（内容感知复用 MiniFilter + 设备控制）。
- [ ] 打印监控（用户态）。
- [ ] 剪贴板 / 截屏（用户态）。

## 阶段 5：可靠审计 + 预留中心化
> 详见 [07](./07-comm-and-audit.md)。

- [ ] 事件面 FltPort 推送 / 批量；持久化续传；背压限流。
- [ ] 取证留存 + 日志防篡改。
- [ ] `PolicySource`/`EventSink` 接口抽象（为 Server 预留）。

## 阶段 6（可选）：网络重定向 / 中心化实现
- [ ] 网络 CONNECT_REDIRECT 到本地 DLP 代理（如需内容改写/深度 MITM）。
- [ ] 文件读重定向 / 透明加密（如需）。
- [ ] Agent-Server 落地（中心策略/事件汇聚/告警报表）。

## 依赖关系

```
阶段0(地基) ─┬─► 阶段1(自保护, 可并行)
             └─► 阶段2(Core+裁决) ─► 阶段3(内容检测) ─► 阶段4(通道)
                                    └─► 阶段5(审计+预留中心化)
                                                        └─► 阶段6(可选)
```

- 阶段 0 是一切前提；阶段 1 可与 2 并行。
- 阶段 3 依赖阶段 2 的异步裁决地基。
- 阶段 6 按业务需要再启。
