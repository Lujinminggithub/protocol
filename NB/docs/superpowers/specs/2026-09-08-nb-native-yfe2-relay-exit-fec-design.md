# NB-native YFE2 Relay-Exit 自适应 FEC 设计

## 目标

为 NB 自有 QUIC 数据面补充与 YFE2 等价的 Relay -> Exit 可选自适应 FEC：正常网络使用系统码 RS `K=16,R=1`，确认物理传输丢包后对后续新 block 切换为 `K=16,R=3`，连续 5 秒没有新增有效物理丢包后自动回落。该能力只保护 Relay -> Exit 的媒体 UDP Datagram，不改变 Entry -> Relay、Exit -> Relay、TCP、产品限速、BBR、媒体调度和开线成功条件。

本设计借鉴 `E:/code/ygtklineAgent/docs/superpowers/specs/2026-09-05-yfe2-relay-exit-fec-canary-design.md` 的行为和安全边界，但不追求与 HY2 YFE2 wire 互通。NB 保留 `NBUD` systematic 数据格式，新增 NB 自有 `NBUF v3` parity envelope。

## 已有能力与差距

NB 当前具备 GF(256) Reed-Solomon、`NBUF v2`、源分片缓存、最多四个 repair、block 边界延迟切换、PMTU 探测和基础恢复计数。现有实现仍有以下差距：

- 只在链路变差后启用，启用时为 `K=8,R=2/3/4`，没有正常期 `16+1`。
- parity 在事件循环同步计算，编码错误可能拆除业务流。
- 没有 interleave、150ms 恢复期限、Profile ID、optional 协商和明确 fallback。
- 有效丢包只扣除 spurious，未排除本地丢弃、PMTU、应用受限、产品限速和探针样本。
- 探针不能完整抑制 parity 和自适应采样。
- 缺少业务/wire 字节、开销、不可恢复、重复、CPU、内存和队列丢弃指标。

`NBUF v2` 保留只读兼容，供升级窗口内的旧 Relay 使用；新 Relay 不再创建新的 v2 编码 session。

## 内置 Profile

Core 内置唯一的 Relay -> Exit Profile：

```json
{
  "enabled": true,
  "codec": "nb-yfe2",
  "wire_version": 3,
  "required": false,
  "direction": "relay_to_exit",
  "data_shards": 16,
  "parity_shards": 1,
  "burst_parity_shards": 3,
  "interleave": 4,
  "flush_ms": 10,
  "recovery_deadline_ms": 150,
  "loss_trigger_packets": 1,
  "hold_ms": 5000
}
```

所有影响 wire 行为的字段按以下无空格 ASCII 文本规范化：

```text
codec=nb-yfe2;wire_version=3;direction=relay_to_exit;data_shards=16;parity_shards=1;burst_parity_shards=3;interleave=4;flush_ms=10;recovery_deadline_ms=150;loss_trigger_packets=1;hold_ms=5000
```

`profile_id` 为该文本 SHA-256 的前两个大端字节，字面值固定为 `28909`（`0x70ed`）。Core、Profile 生成器和测试必须独立断言该值，运行配置不得手工覆盖。

## 启用范围与配置所有权

新增 transport profile `schema=2`。Core 仍接受现有 `schema=1`，其 FEC 行为保持不变。`schema=2` 只增加一个枚举开关：

```text
egress.udp_fec_mode=nb-yfe2-optional
```

它只允许出现在 Middle egress；Exit 对 `nb-yfe2` acceptance 的支持来自新版 Core，不需要线路字段。Entry、Middle ingress 和非 `schema=2` Profile 不发送 proposal。

控制面数据库和开线请求不新增 FEC 字段。新建线路的 Profile 生成器内置上述值并为 Middle egress 写入 optional mode。现有线路必须在明确维护或灰度操作中升级到 `schema=2`，不能因二进制升级自动启用。

## Optional 协商

### 控制通道

不修改 TLS ALPN 或 QUIC transport parameter。每个新 Relay -> Exit QUIC connection 建立后，Middle 通过合法的标准 `NBUD C2S` Datagram 向保留目标发送 proposal：

```text
T:nb-fec-capability.internal:9
```

payload 使用固定长度 `NBFC v1`，包含消息类型、64 位随机 nonce、`profile_id` 和全部 wire 字段。新版 Exit 在 DNS 前截获该精确目标，验证 canonical Profile 后，通过同一 connection 返回标准 `NBUD S2C` acceptance，回显 nonce 和 Profile。Middle 只接受同一 connection、同一 nonce、精确 Profile 的响应。

保留目标不加入手机白名单，Entry 不允许客户端生成该请求。Exit 只在目标完全等于保留域名且端口为 9 时进入协商处理；其他相似名称按普通规则处理。

### 状态机

每条 QUIC connection 独立维护：

```text
disabled
proposed
accepted
fallback_timeout
fallback_not_supported
fallback_profile_mismatch
fallback_pmtu
fallback_resource
```

proposal 不阻塞业务 Datagram。500ms 内没有 acceptance 即进入 `fallback_timeout`，该 connection 的 systematic 数据继续使用标准 `NBUD`。旧 Exit 会把保留目标作为一个独立 UDP flow 处理并最终 DNS 失败，但不能关闭共享 QUIC connection。

`required=false` 为固定不变量。任何解析、Profile、PMTU、分配或编码问题只能禁用当前 connection 的 v3 parity并记录原因，不能拆除业务流、改变线路 health 或触发开线回滚。只在自然重连或明确维护时重新协商。

## `NBUF v3` Wire

systematic 分片继续按原 `NBUD v1` 立即发送，不等待凑满 block，也不增加 envelope。只有 parity 使用 `NBUF v3`。固定头为 24 字节：

| Offset | 长度 | 字段 |
|---:|---:|---|
| 0 | 4 | magic `NBUF` |
| 4 | 1 | wire version `3` |
| 5 | 1 | flags；仅允许 `BURST=0x01` |
| 6 | 2 | `profile_id=0x70ed`，大端 |
| 8 | 4 | session ID，大端 |
| 12 | 4 | block ID，大端 |
| 16 | 1 | direction；本轮只允许 C2S |
| 17 | 1 | shard index；parity 为 16..18 |
| 18 | 1 | K，必须为 16 |
| 19 | 1 | 本 block 的 R，只允许 1 或 3 |
| 20 | 1 | actual data count，1..16 |
| 21 | 1 | reserved，必须为 0 |
| 22 | 2 | shard size，大端 |

固定头后跟 `actual data count` 个 12 字节 descriptor：sequence 4字节、fragment index 2字节、fragment count 2字节、total length 2字节、payload length 2字节，最后为 RS parity body。

route 不在每个 parity 中重复；Exit 由同一 connection 上的 session ID 找到已认证的业务 flow。decoder 同时接受同一 Profile 的 `16+1` 和带 `BURST` 的 `16+3`，拒绝其他 K/R、未知 flags、错误 Profile、越界长度和损坏 shard。`NBUF v2` 继续由原 decoder 独立处理，不得与 v3 block 混合。

发送前按该 connection 实际 QUIC Datagram payload 上限计算完整 v3 frame。picoquic prepare callback 当次提供的 `length` 只是当前包剩余 allowance；若它小于 parity 但连接上限足够，必须保留独立 parity 队列并请求下一包，不能误判 PMTU。只有完整 frame 超过连接上限时才将该 connection 标记 `fallback_pmtu`；不得缩短原始 `NBUD`、拆 parity 或降低业务 MTU来强行启用。

## 编码数据路径

每个媒体 UDP flow 维护四个交错 block。连续 systematic 分片轮流进入四个 block，使相邻丢包分散。block 满 16 个时立即封口；整个 flow 连续 10ms 没有新 systematic 时刷出全部 partial block；持续有流量时单个 block 最长 150ms 封口。partial block 的缺位以全零 shard补足 K=16参与 RS，但 wire 只携带实际 descriptor count；decoder将未使用位置视为已知零 shard，只恢复和交付实际 data count，不向业务端交付 padding。

这里的 `flush_ms=10` 明确定义为 flow 空闲间隔，不是从 block 首包开始的硬期限。2026-09-08 首次 worker 0 canary 证明“首包后 10ms”在四路交错和低包率下会退化为接近每个原包一个 parity，实测开销 172.7%，因此被验收门禁拒绝并回滚。150ms 持续流上限用于同时约束恢复等待和正常期开销。

原始路径顺序固定：

1. Middle 将原 `NBUD` 加入正常 QUIC 媒体队列。
2. 成功入队后，将源分片元数据和 payload 复制到 FEC block。
3. block 封口后提交到进程级有界 parity worker。
4. worker 异步执行 RS 编码，将结果送回事件循环的低优先 parity 队列。
5. parity 只有成功进入 QUIC 队列才计为 sent/wire bytes。

编码线程不得直接访问 picoquic connection、flow 指针或可变队列。任务使用不可变快照和 connection generation token；结果返回时 generation 不匹配则丢弃。worker queue 满、内存不足、编码失败或 parity 队列满只丢 parity，原始包已经发送且不得撤销。

资源上限：每 worker 最多 128 个待编码 block、16MiB 编码任务内存；每 flow 最多 8 个未完成/待回收 block；Exit 每 flow 最多 32 个恢复 block和 2MiB 缓存，进程全局最多 256 个恢复 block和 32MiB。超过上限淘汰最旧 incomplete block并计数，不影响新 systematic 的直接交付。

## 解码和恢复

Exit 收到 systematic `NBUD` 时立即走原业务路径，同时按 `(connection generation, session, direction, descriptor)` 缓存用于恢复。收到 v3 parity 后：

- Profile 和 block 元数据必须精确验证；
- 已直接交付的 systematic 标记为 delivered；
- 可用 shard 数达到实际 data count且缺失数不超过 R 时执行恢复；
- 恢复出的 `NBUD` 重新经过原解码、重组和目标发送路径；
- 每个 descriptor最多交付一次，迟到 systematic 和重复 parity 只计 duplicate；
- block 首次可恢复等待超过 150ms 后标记 unrecoverable并释放资源；
- 超时或淘汰后到达的未见 systematic 仍直接交付，不能因 FEC 状态丢弃原包。

恢复计算发生在事件循环，但单次最多 `16+3` 个 1000-byte shard；记录 CPU 时间。若生产证据显示单次恢复超过 2ms P99，再将 decode 移到独立 worker，本轮不提前增加双异步复杂度。

## 自适应状态机

每个已 `accepted` 的 Relay -> Exit connection 独立维护：

- `BASELINE`：新 block 使用 `16+1`。
- `BURST`：新 block 使用 `16+3`。

每 200ms读取累计传输计数差值；计数回退或 connection generation 变化时只重建基线，不触发。有效物理丢包计算必须扣除 spurious，并在以下任一条件成立时丢弃整个采样窗：

- 本地 UDP `SO_RXQ_OVFL`、应用发送错误或 FEC/业务队列丢弃增长；
- scheduler expiry/send queue full增长；
- PMTU blackhole/fallback 发生；
- connection 为 app-limited；
- 租户产品令牌发生等待，处于 rate-cap-limited；
- 样本来自内置开线/周期探针。

Picoquic 必须通过只读适配接口暴露 app-limited、send queue full和累计路径丢包；租户 limiter 暴露当前 connection 的限速等待。任一信号不可可靠读取时，该窗口标记 invalid，不能猜测为物理丢包。

第一个有效丢包使后续创建的新 block 进入 `BURST`，并将 `burst_until` 设为当前时间加 5 秒。后续有效丢包刷新期限。连续 5 秒没有新增有效丢包后回到 `BASELINE`。切换只影响新 block；active、ready 或 worker queue 中的 block 保持创建时 R。

## 探针隔离

所有 `nb-probe-*.internal:9` 流继续使用标准 `NBUD`，即使 connection 已协商 v3：

- 不加入 FEC block；
- 不生成 parity；
- 不进入 decoder恢复缓存；
- 不更新有效丢包、自适应模式或 5 秒保持期；
- 探针按业务 payload 计算吞吐与完整性。

探针前后 `current_mode` 和 `burst_until` 必须一致，诊断增加 `probe_parity_suppressed` 和 `probe_samples_ignored`。

## 计量和调度

- 产品业务字节、租户配额和 5Mbps/其他套餐限速只统计原始 payload；recovered payload 不重复计费。
- systematic `NBUD` 使用原媒体优先级和原 deadline。
- parity 使用 `NB_PRIO_FEC=12`，低于 systematic 媒体 `4`、高于普通 Bulk `20`；媒体队列有待发数据时 parity 不得抢占 systematic。
- parity 计入 QUIC pacing、YFE2 wire bytes、CPU和内存，不计入业务吞吐门禁。YFE2 `wire_bytes` 定义为原始业务 payload 加 parity frame，用于计算 FEC 增量开销；既有 NBUD/QUIC framing 不重复归因给 FEC。
- FEC不得修改 BBR状态、cwnd seed、PMTU搜索、媒体分类或租户媒体保底。

## 诊断

现有 `metrics` 保持字段兼容，并扩展 aggregate；新增只读 `GET /fec` 返回有界的 connection 列表。每条 connection 至少包含：

- negotiation state、codec、wire version、profile ID、fallback reason；
- current mode、burst transitions、last trigger、burst until；
- original、baseline parity、burst parity、received、recovered、unrecoverable、duplicate、corrupt；
- business bytes、wire bytes、overhead ratio；
- encode/decode CPU ns、任务和结果队列高水位、内存高水位；
- encoder/decoder/parity queue drops、PMTU fallback；
- effective physical loss、invalid sample counts及按原因拆分；
- probe parity suppressed、probe samples ignored。

列表最多返回当前 worker 的 pool connection 数，不扫描日志。connection关闭后，其累计值合并到 worker lifetime aggregate。

## 开线隔离

FEC状态不得进入任何开线硬门禁：

- line request、operation plan和数据库不新增 FEC 必填字段；
- proposal/acceptance、fallback、parity、恢复率和 Profile readiness 不影响 active；
-可靠 QUIC 90% 带宽门禁、UDP完整性门禁和现有超时保持原语义；
- FEC清理失败不能占用开线锁或阻止线路删除；
- FEC代码失败只影响 parity，不能造成 original queue drop或业务 flow teardown。

## 升级与灰度

顺序固定：

1. 发布支持 `NBUF v2/v3` 解码和 `NBFC acceptance` 的 Exit 二进制，`schema=1` 行为不变。
2. 在维护窗口重启目标 Exit shard；验证旧 Relay 的标准 NBUD/v2兼容。
3. 发布支持 proposal、异步编码和自适应的 Middle 二进制，但 Profile 仍为 schema 1。
4. 为同编号的一对 Middle/Exit shard发布 schema 2 canary Profile；另一 shard保持 schema 1作为对照。
5. 只让新 connection进入 canary；已有 connection不切换 wire mode。
6. 验收后扩到第二 shard。失败时先把 Middle canary Profile恢复 schema 1，停止产生新 parity，再回滚二进制。

Entry 不需要为本能力重启。若部署工具无法保证 Middle/Exit同编号 shard配对，则禁止启用 canary，不能随机把 proposal流量发往非目标 Exit shard。

## TDD 与验证矩阵

实现必须先 RED 后 GREEN，按以下顺序：

1. Profile：canonical ID固定为 28909；schema 1不启用；schema 2只允许 Middle egress optional。
2. 协商：新新 accepted；新 Relay/旧 Exit超时 fallback；Profile mismatch fallback；伪造 nonce拒绝；业务始终继续。
3. Wire：v3合法往返；未知 flag、K/R、Profile、length、shard index和损坏数据拒绝；v2/v3不混合。
4. 编码：systematic立即入队；无损严格 `16+1`；四路 interleave；10ms partial flush；parity失败不影响 original。
5. 自适应：第一个有效丢包使下一个 block进入 `16+3`；已创建 block不变；5秒回落；计数回退重建基线。
6. 过滤：spurious、本地丢弃、queue expiry、PMTU、app-limited、rate-cap-limited及 probe都不触发或刷新 BURST。
7. 恢复：丢失不超过 R 时原 payload只交付一次；超过 R、150ms超时、损坏或资源淘汰明确失败；迟到 systematic仍交付。
8. 资源与并发：至少 6 路持续 30秒；任务/结果/decoder队列上限；无泄漏、竞态、event-loop阻塞或 original停滞。
9. Linux namespace/netem：0、0.1%、0.2%、0.5%、1%随机丢包，1/3/6/12包突发，重排、policer、app-limited和rate-cap-limited。
10. 回归：Entry -> Relay、Exit -> Relay、TCP、媒体分类、租户保底、产品限速、BBR、PMTU和开线探针结果不变。

## 验收标准

- optional FEC任何失败均不导致开线失败、共享 QUIC关闭或业务流拆除。
- 完整升级的 canary connection 为 `accepted`，无损期只使用 `16+1`。
- 无损30分钟不进入 BURST，业务吞吐不低于 FEC-off基线的98%，original queue drop为0。
- 约1000-byte systematic流量的正常 wire开销实测在6%至8.5%；以 `wire_bytes/business_bytes` 为准。
- 注入有效物理丢包后，下一个新 block使用 `16+3`；稳定5秒后回到 `16+1`。
- 1包随机丢失可恢复；3包突发在同一 interleave block不超过R时恢复；超过R明确计为unrecoverable且不重复交付。
- 探针 parity为0，探针前后自适应状态不变，门禁结果与FEC-off一致。
- Entry -> Relay、Exit -> Relay、TCP和schema 1线路的wire bytes与升级前一致。
- 6路并发30秒中 event-loop busy P99不恶化超过1ms，编码队列无持续积压，内存保持在声明上限内。
- 全部单元、集成、race/TSAN可用检查和Linux netem矩阵通过后，才允许从单shard扩面。
