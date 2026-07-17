# 07 通信、事件管道与审计

> 上级：[README](./README.md) ｜ 相关：[03 异步裁决](./03-async-adjudication.md)、[06 自我保护](./06-self-protection.md)

## 现状

- 通信：IOCTL（`device.c`），用户态**单条轮询**拉取事件，`Dequeue` 的 TimeoutMs 被忽略（忙轮询）。
- 事件：静态环形缓冲（`file_event.c`/`net_event.c`），**满即静默丢弃、无丢弃计数**；事件定长（`DLP_FILE_EVENT`~1.6KB、`DLP_NET_EVENT` 更大），64KB 缓冲仅容数十/数条。
- 命名管道 `pipe_manager` 为空桩。
- 无持久化、无取证、无中心化上报。

## 目标：可靠事件管道（审计不丢是红线）

### 通信通道
- **控制面**（策略下发、裁决回写、自保护）：IOCTL，同步语义。
- **事件面**（内核→用户态推送）：改用 **FltMgr 通信端口** `FltCreateCommunicationPort` + `FltSendMessage` 主动推送；或至少 IOCTL **批量出队 + KEVENT 唤醒**，消除忙轮询。
- 双向：裁决请求（内核→用户态）与裁决结果（用户态→内核）见 [03](./03-async-adjudication.md)。

### 事件管道可靠性
| 问题 | 目标 |
|---|---|
| 满即丢 | 增大/多页缓冲 + **丢弃计数**(`InterlockedIncrement`)经 `DRIVER_STATUS` 上报 |
| 定长浪费 | 头部定长 + 变长尾部紧凑序列化 |
| 单条轮询 | 批量出队 / 主动推送 |
| 无背压 | 高水位对**审计类**降采样，**阻断类**始终入队（独立高优先级）|
| 无持久化 | 用户态落盘队列，断电/重启续传 |

### 限流/背压（内核侧，当前完全缺失）
- 令牌桶按事件类型分级：**审计/日志类可采样丢弃；阻断/裁决类绝不限流**（安全决策必达）。
- 「是否阻断」与「是否上报」解耦：阻断永远执行，上报走令牌桶。

## 审计与取证
- 事件持久化（用户态 DB/文件），支持查询/检索。
- **取证留存**：命中时保留证据副本/快照（可复用隔离机制）。
- 日志防篡改：与[自保护](./06-self-protection.md)统一（保护日志文件/目录）。
- 关键动作（解锁/重锁/拦截/裁决）全审计。

## 预留中心化接口（ADR D3：端侧为主，预留不实现）

用接口抽象隔离「端侧 vs 中心化」，将来接 Server 只是「换实现」而非重构：

```
策略源接口 PolicySource:
   getPolicy() / onPolicyUpdate()
   实现①: 本地策略文件(现在)    实现②: Server下发(预留)

事件汇聚接口 EventSink:
   emit(event) / flush()
   实现①: 本地落盘/DB(现在)     实现②: 上报Server(预留)

Agent 身份/心跳接口(预留): register() / heartbeat()
```

## 正确性
- 卸载：`IO_REMOVE_LOCK` 保证无在途 IOCTL/回调后再释放队列；先删符号链接挡新 IRP。
- 通信端口/管道在卸载时按序关闭。

## 待办

- [x] 事件面先升级为批量+唤醒：内核支持 batch dequeue，用户态支持 wait + batch drain。
- [x] 事件管道第一版：丢弃计数、批量出队、KEVENT 唤醒、本地持久化续传队列、本地 HTTP sink、端到端投递验证。
- [ ] 内核限流（审计采样 / 阻断不限流）。
- [ ] 取证留存 + 日志防篡改。
- [ ] `PolicySource`/`EventSink` 接口抽象（预留中心化）。

## 涉及文件

- 内核：`src/kernel/device.c`、`src/kernel/filter/file_event.c`、`src/kernel/network/net_event.c`、`src/kernel/core/ratelimit.*`（新增）
- 用户态：`src/native/src/dlp/`（推送接收/持久化/上报接口）
