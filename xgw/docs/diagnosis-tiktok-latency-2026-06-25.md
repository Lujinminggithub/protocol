# 诊断：TikTok bootstrap 时延 —— 前端 CPU 饱和 + 隧道 bufferbloat（2026-06-25 现网实测）

> 现象：TikTok 相关 **TCP** 到了前端/relay/egress，但 bootstrap 关键控制流首字节
> 拿不到或要 1.5~5s+，App 在进入 UDP 前就把网络判死，前端始终看不到 TikTok UDP。
>
> 本文基于 2026-06-25 16:00~16:07 登录三跳（ingress=gz-170 edge / relay=hk-176 经 edge 跳板私网 /
> egress=kz-1）实采日志（`tmp/diag/*`）给出**量化的逐跳时延拆解**与根因排序。

---

## 0. 一句话结论（2026-06-25 perf 实证后修订）

整条链路有**两个独立的 CPU 饱和点**，都随 **TikTok bootstrap 的并发流数 N** 暴涨（bootstrap 一次拉 10+ 条控制流），与字节流量无关：

1. **后端 C 端（relay/egress，perf 已一锤定音）**：`src/runtime.c` 的流调度器 `stream_sched_pick`
   是 **O(N²)~O(N³) 的字符串比较**热路径。perf 显示 relay 99.9% CPU 中 **36%+ 落在
   `stream_sched_group_pending_count` / `stream_sched_total_weight` / `stream_sched_same_route_group`，
   另 ~30% 落在它们内部的 libc `strcmp/memcmp`**。N 一大（TikTok 进来）→ relay 单核打满 →
   来不及 drain → 队列堆积 → `srtt≈2s`（min_rtt 仅 4-17ms）的 bufferbloat → 这就是
   `egress→relay`、`relay→ingress` 延迟高的**真实物理来源**（不是跨境链路差）。
2. **前端 Go 端（gz-170 edge，xgw-edge-server，perf 已一锤定音）**：186% CPU（2 核全满），strace 证明全在用户态。
   perf 调用图显示 **98.6% CPU 在 `copyFrontToBridge`，其中 75.6% 在 `isTimeoutErr → errors.As → reflectlite`（反射）**。
   根因是 `internal/officialbridge/server.go` 的 `copyFrontToBridge`(:1173) / `officialBridgeConn.Read`(:902)
   在 `if isTimeoutErr(err) { continue }` 上**忙等自旋**：我们**从不设 read deadline**，故 Read 的超时只能是
   QUIC 连接级 idle/终止错误（终态），却被当可重试 → 对死/idle 连接无限重读（`sharedRingConn.readTimeout`
   过期返回 1ns → `time.After` 立即触发，纯用户态自旋、无 syscall），每次还做 `errors.As` 反射。
   **idle 掉的手机连接会各留一个 100% 自旋的 goroutine 并累积**，所以**零流量时 CPU 仍 186%**。
   后端 C 层 ingress 在 **p50 1.6s** 拿到回程首字节，但前端 **p50 ~20s** 才交付给 App，**~18s 堆在前端**；
   约 **40% 的 bootstrap TCP 在 App 30s 超时窗内 `first_byte_ms=-1`** → App 判无网 → 不发 UDP。

> **重要更正**：本文初版把前端 186% 归因为"`shm-direct` 共享内存环 busy-poll 自旋"，
> **此结论已被代码证伪**：`bridge_ring_mmap_linux.go` 的 `syncBridgeRingMapping` 是 no-op，
> `waitForChange` 用真正的 `FUTEX_WAIT/WAKE` 内核 park（`ringNotifyHasKernelWake()=true`）。
> 环本身不自旋，前端 CPU 另有出处（见上 ⚠️）。

---

## 0b. 修复与部署结果（2026-06-25 已部署线上，决定性验证）

两个 perf 实证根因均已修复、编译/对拍通过并部署到线上（C 三跳 egress→relay→ingress + 前端）：

| 指标 | 修复前 | 修复后（部署即时） | 结论 |
|---|---|---|---|
| **front CPU** | **186%**（零流量也满载） | **0.0%** | 根因① 反射自旋修复，死连接 goroutine 不再累积 |
| **relay CPU** | **77%** | **1.8%** | 根因② O(N) 调度器，strcmp 风暴消除 |
| egress / ingress CPU | 高 | 0.7% / 1.8% | 同 O(N) 受益 |
| **relay srtt / min_rtt** | 2,031,637 / 4,555（**445×**） | 226,224 / 225,812（**≈1.0×**） | bufferbloat 消失，srtt 贴住真实 RTT |

- 修复①（前端，Go）：`copyFrontToBridge`/`officialBridgeConn.Read` 两处 `isTimeoutErr→continue` 自旋
  改为传播超时让连接关闭；删除反射版 `isTimeoutErr`。`go build`/`go vet` 通过。
- 修复②（后端，C）：`stream_sched_pick` 每次预聚合分组量（FNV64 代际哈希表 + 容斥）一次 O(N)，
  循环内 `total_weight`/`group_*` 改 O(1) 查表，消灭热路径 strcmp。旧实现保留为 `_ref`，
  新增 `xgw_runtime_sched_selftest` 随机 40 轮对拍逐值等价（`selftest.sched_agg=ok`），
  `cc -Wall -Wextra -pedantic` 零告警。
- shm-direct **保留**（扛大流量必需）；srtt=100000 是 BBR startup 占位；relay/egress srtt≈226ms 的会话
  其 min_rtt 本就 225ms（到 KZ 真实跨境 RTT），srtt≈min_rtt = 健康。
- **待真实负载验证**：前端 `first_byte`（应从 p50~20s 降到秒级、-1 超时率从 40% 下降）需 TikTok 流量恢复后复测，
  用 `tmp/diag_verify.py`。

### 0c. 复测进展（2026-06-25 18:50 前端重启后）

- CPU 持续低位：front 0.7%、relay 4.2%、egress 1.2%、ingress 2.3% —— **CPU 修复稳定生效**。
- 前端修复**无副作用**：`flow.close` reason 分布只有 `udp_close_called`（2，正常 NTP），
  **零个 TCP 异常/超时关闭**，证明「超时不再 continue」没有把正常短读误当终态。
- 已观测到的 TCP 成功 `first_byte_ms = 435 / 441ms`（**秒级以内**，修复前 ~20s）。
- 两个 `first_byte_ms=600000` 是 **UDP NTP**（`time.apple.com:123`, `reason=udp_close_called`），
  600000=UDP idle 上限封顶，NTP 单包静默，非故障。
- **仍缺 TikTok 实测样本**：前端重启后 `req_tiktok=0`（还没人用 TikTok 测）。最终验证需用真实 TikTok
  bootstrap 触发，确认 TikTok 域名（api-boot/mon-boot/frontier…）的 `first_byte` 秒级、`-1` 超时率显著下降。

---

---

## 1. 逐跳时延拆解（回程首字节方向，bootstrap 最敏感）

采样：TikTok 相关流 `*.phase.first_byte` / `tiktok.trace.*` / 前端 `hy2front.flow.close`。

| 观测点 | 含义 | p50 | p90 | p99/max | 备注 |
|---|---|---|---|---|---|
| egress `connect_ms` | KZ→真实 TikTok 上游 TCP 建连 | **97ms** | 287ms | 447ms / 1650ms | **健康**，落地建连没问题 |
| egress `first_byte` | 真实上游响应首字节(自 egress 开流) | 645ms | **4962ms** | 7269ms | KZ→TikTok 边缘本身 p90 就 ~5s |
| relay `first_byte` | 回程到达 HK relay | 1530ms | 7719ms | 7797ms | 比 egress +~885ms（KZ→HK 段+排队） |
| ingress `first_byte` | 回程到达 GZ 后端 C 层 | 1605ms | 7755ms | 7810ms | 比 relay +~75ms（HK→GZ 段 p50 很便宜） |
| **front `first_byte`** | **前端交付给 App** | **~20313ms** | 600000(封顶) | 600000 | **比 ingress +~18s，且仅统计成功流** |
| **front `first_byte=-1`** | **拿不到首字节直接超时** | — | — | — | **26/65 ≈ 40%**，`lifetime≈30s` 后 App 关流 |

**结论**：时延预算里，跨境三段（egress→relay→ingress）p50 合计仅 ~1s，**真正的 ~18s 黑洞在
`ingress(C) → 前端 → App` 这一段**，即前端进程。

---

## 2. 根因 A（决定性）：边缘机 CPU 饱和，前端把首字节交付拖到 ~20s / 直接超时

证据（`tmp/diag/` + `diag_front.py` 实时快照）：

- `nproc = 2`（gz-170 只有 **2 vCPU**），`loadavg = 1.96/2.01/2.00`，`%Cpu(s): 93.3 us / 6.7 id`
  —— **两核常态打满**。
- `top`：`xgw-edge-server` **%CPU=186.7**；线程级两条 Go 线程 **99.9% + 93.3%**。
- 前端配置 `hy2-front.json`：`"bridge_transport": "shm-direct"`、`"bridge_ring_path": "/run/xgw/bridge-ring"`。
  ~~初版认为这里 busy-poll 自旋~~ —— **已证伪**：`bridge_ring_mmap_linux.go` 的 `syncBridgeRingMapping`
  是 no-op、`waitForChange` 走真 `FUTEX_WAIT`，环不自旋。前端 186% 的具体热点函数**待 perf 补测**。
- 前端日志：成功流 `first_byte_ms` 普遍 4834/5786ms 起步、中位 ~20s；失败流
  `first_byte_ms=-1 first_packet_ms=0 lifetime_ms≈30000 reason=tcp_close_called`
  —— **30s 是 App 自己的 bootstrap 超时**，到点 App 主动关流。
- App bootstrap 一次性并发拉起 ~10+ 条 TCP（同一毫秒内对 api-boot/mon-boot/frontier/bsync/libra-boot…），
  在 2 核被自旋占满的情况下，这些流的回程字节排队 → 大面积 5~20s 或直接 -1。

**机理**：CPU 无余量 → HY2 core 的 QUIC 收发/加解密 + shm 环搬运抢不到时间片 →
回程首字节延迟爆炸 → ~40% 流撞上 30s 判死 → App 认为无网 → 不再发起 UDP（与前端 `udp count=0` 自洽）。

### 修复 A（按代价排序）

1. **立刻**：把前端 bridge 从 `shm-direct`（自旋）换成**事件驱动/阻塞式**通道（如 unix socket / TCP `127.0.0.1:19080`
   的阻塞读写），消除空转自旋；2 核机器尤其不能用 busy-poll。
2. **立刻**：给边缘机扩到 **≥4 vCPU**，或把前端与 `/opt` 旧栈分到不同机器（见根因 C）。
3. 降低前端热路径日志量（`tiktok.trace.*`/`flow.close` 每流多行 JSON，verbose 在满载时进一步抢 CPU）。
4. 确认 HY2 core 是否开了不必要的逐包加密/校验放大。

---

## 2b. 根因 A2（perf 一锤定音）：C 端流调度器 `stream_sched_pick` 是 O(N²)~O(N³) 字符串比较

> 这是 relay/egress（以及 ingress 在高并发时）CPU 高的**确定根因**，由 perf 现网采样直接归因。

perf 采样 relay（hk-176，本机自带 perf）`xgw` 进程 12s（2375 样本），self-time top：

```
15.66%  stream_sched_group_pending_count.part.0
13.68%  stream_sched_total_weight.part.0
 6.82%  stream_sched_same_route_group.part.0
~30%    libc.so.6  0x1b18xx 连续地址段（= 上述函数内部的 strcmp/memcmp）
 3.07%  xgw_runtime_run
```

即 **>36% CPU 直接在三个 `stream_sched_*`，叠加 ~30% 的 libc 字符串比较 → 约 2/3 的 relay CPU 在调度器分组计算**。

**复杂度链**（`src/runtime.c`）：

- `stream_sched_pick`（:823，每次 drain 调一次，只挑 1 条流发）外层遍历全部活跃流 `active_n` → **O(N)**。
- 每条流内部触发约 **9 次 O(N) 全表扫描**：`refill_credit`(:858) 内含 `budget_bytes`+`refill_rate`，
  二者各调 `total_weight`(:1159, O(N)) + `group_pending_count`(:1112, O(N))；外加 `refill_rate`(:859)、
  `budget_bytes`(:868)、`group_inflight_bytes`(:871)、`budget_bytes`(:872) 反复重算。
- 每次扫描的每个元素再做最多 **2 次 `strcmp`**（`stream_sched_same_route_group` :1087-1097 比 `route_name`/`line_id`）。

合计 **≈18·N² 次 strcmp / 次 pick**；drain 一轮 pick N 次 → **≈O(N³) strcmp / drain**。

**为什么和你的现象完全吻合**：
- relay 单线程 270 包/s 却 **2.85ms/包**（比 1200B 包 AEAD 高 3 个数量级）——成本由**并发流数 N**（立方）决定，非字节。
- TikTok bootstrap 同一毫秒并发拉 10+ 条控制流 → N 陡增 → 调度成本立方爆炸 → relay 单核打满 →
  drain 跟不上 → 队列堆积 → `srtt≈2s` vs `min_rtt 4-17ms` 的 bufferbloat（§3）。
- 空闲（N 小）不卡，TikTok 一进来（N 大）立刻卡。

### 修复 A2（直接解决，按收益排序）

1. **每次 `pick` 把分组量算一次、缓存复用**：`total_weight`、`group_pending_count`、`group_inflight_bytes`
   在单次 `pick` 内对同一 (line_id,direction[,group]) 是**常量**（活跃集不变）。在 `pick` 开头做**一遍 O(N) 预聚合**，
   按 group 累加权重/pending/inflight 到小表，循环内 O(1) 查表。→ O(N³) 降到 **O(N)**。
2. **用整型 group key 取代 strcmp**：入队时把 `route_name`/`line_id`/`front_session_id` **intern 成整数 id**
   （或预存 hash），分组比较改为整型 `==`，消灭热路径上全部 `strcmp/memcmp`（perf 里那 ~30% libc）。
3. **去掉每流多次重复调用**：`budget_bytes` 与 `refill_rate` 内部都各算一遍 `total_weight`+`group_pending_count`，
   合并为一次计算并传入。

预期：relay/egress 在 TikTok 并发下 CPU 从打满降到个位数%，drain 跟得上 → bufferbloat（§3）随之大幅缓解。

> 注：ingress/egress 跑同一份 `runtime.c`，同样受益；edge 上 C ingress 采样时恰逢低负载（0% CPU），
> 但高并发时会同样触发该 O(N³)。

---


证据（各跳 `runtime.cc`）：

| 跳 | srtt_us p50 | min_rtt_us p50 | 说明 |
|---|---|---|---|
| egress(KZ) | **1,958,773 (~1.96s)** | 17,134 (~17ms) | 实际链路 RTT 仅 17ms，却堆了近 2s 队列 |
| relay(HK) | **2,031,637 (~2.03s)** | 4,555 (~4.5ms) | 同上 |
| ingress(GZ) | **1,694,835 (~1.69s)** | 4,531 (~4.5ms) | 同上 |

`pacing_bps` 实测被拉到 **200~250 Mbps**（配置 `profile=live-bbr` 名义 50Mbps），且见到单会话
`cwnd` 涨到 **11MB**。**发送侧 pacing/cwnd 远超瓶颈带宽 → 自填队列 → srtt 从 ~5-17ms 膨胀到 ~2s。**
这就是 `egress→relay`、`relay→ingress` 看起来“延迟高”的真实来源：不是物理链路差，是**自己把队列灌满**。

### 修复 B

- 给 BBR/stream_sched 的 pacing 与 cwnd 设**基于 min_rtt 的上限**（inflight ≤ bw×min_rtt × 小倍数），
  别让 cwnd 涨到 MB 级；或把 `pacing_bps` 钳回与实际带宽匹配（先试 ≤50Mbps）。
- 隧道层加 AQM/队列上限，srtt 偏离 min_rtt 数十倍时果断降速。

---

## 4. 根因 C（环境卫生）：每台机并存两套 xgw，抢 CPU

- 每个节点同时在跑：`/opt/xgw/xgw-current ... :6000`（Jun24 旧 sing-box 栈）+ `/etc/xgw/xgw ... :51840`（当前 HY2 栈）。
- 端口不冲突（6000 vs 51840），但在 2 核机器上**白白多占 CPU**，加剧根因 A 的饱和。
- egress 跑在 **UTC** 时区（日志 08:03Z = 北京 16:03），并非停摆——排除“egress 不工作”的误判。

### 修复 C

- 现网只保留 `/etc/xgw` HY2 栈，停掉 `/opt/xgw` 旧 sing-box 栈（释放 CPU）。
- 严守 `egress→relay→ingress` 重启时序（当前 egress 07:16 / relay+ingress 15:17 是分时段重启，
  虽未直接致命，但与文档 §5 时序要求不符，建议整体重启对齐）。

---

## 5. close reason 复核（澄清“close_from_egress 是不是故障”）

- relay：`close_from_egress = 174/174`；egress：`target_recv_eof = 181`、`target_recv_fail=9`、
  `return_queue_full=9`、`target_connect_fail=1`。
- **大部分 `close_from_egress` 其实是正常短请求完成**：上游回完数据后 EOF（`target_recv_eof`）→ egress 关流 →
  向上游传播为 relay 的 `close_from_egress`。**不等于失败**。
- 真正异常是少量 `return_queue_full`（9）：egress→relay 回程队列被打满 →
  与“下游（relay→ingress→前端）抽不动”一致，正是根因 A（前端抽不动）在 egress 侧的背压投影。

---

## 6. 处置优先级

| 优先级 | 动作 | 根因 | 预期效果 |
|---|---|---|---|
| **P0** | **C 端调度器去 O(N³)**：`stream_sched_pick` 单次预聚合分组量 + 整型 group key 取代 strcmp（§2b 修复 A2） | A2 | relay/egress CPU 从打满降到个位数%，drain 跟上 |
| **P0** | 前端 Go 进程补一次 perf/pprof，定位 186% 的热点函数（edge apt 装 perf 失败，待补） | A | 关闭前端 CPU 的最后未知项 |
| **P0** | 停掉 `/opt/xgw` 旧栈，释放边缘机 CPU | C | 给前端腾出核 |
| **P1** | pacing/cwnd 基于 min_rtt 钳制，消除 srtt≈2s bufferbloat（A2 修好后多半自然缓解） | B | 跨境每跳省 ~1-2s |
| **P2** | `runtime.c:3481` 每轮对 unconnected UDP 调 `getsockopt(IP_MTU)` 必失败（1080/1080 err），改为按秒节流/跳过 | 清理 | 去掉无效 syscall |
| **P2** | 整体按 egress→relay→ingress 重启对齐时序；降低热路径日志量 | A/C | 稳定性 |

> 核心判断：**App 判“无网”是因为 bootstrap TCP 首字节被前端 CPU 饱和拖过 30s，而非 UDP 通道坏。**
> 先把前端 CPU 解放（P0），bootstrap 首字节回到秒级，App 才会继续走到 UDP 阶段。
