# NB 已实现功能详解（开发文档）

> 本文保留早期故障根因作为历史记录。当前 V1.5 已使用 `prepare_to_send`、环形队列、epoll 和多连接池，现状以 [12-V1.5实现状态与回归](12-V1.5实现状态与回归.md) 为准。

> [01-需求](01-requirements.md) | [02-架构](02-architecture.md) | 03-已实现(本篇) | [04-部署运维](04-deployment-ops.md) | [05-路线图](05-roadmap.md)

每项含：问题现象 → 根因 → 方案 → 验证。核心代码 `src/nb_node.c`。

## A. 雪崩/流泄漏根治（最关键）

**现象**：并发 20 HTTPS 压测，第一批 6/20 成功其余 000，第二批全 000；entry 存活但 `ss :端口` 74 连接堆积。手机 TikTok 打开即"无网络连接"。

**诊断（数据驱动）**：`nb_diag.py` 抓三跳日志差量 → entry `CONNECT=157 但 PSFREE=36`（121 条流泄漏），middle 零泄漏。coldtest：新鲜 entry `round1=11/20→round2=0→round3=0`，`CONNECT=61 PSFREE=1`（连成功的流也泄漏）。

**根因（三环）**：
1. `maybe_free` 死等"双向 FIN 齐全"，entry 的 `down_fin_seen` 依赖 target 主动关 TCP。真实服务器(HTTP keep-alive)响应完不关 → 回程 FIN 永不来 → **成功的流也 100% 泄漏**。
2. 所有流复用**单条** down_cnx，泄漏流占死连接级 flow-control 额度 → add_to_stream 发不出（middle Δ=0，数据一字节到不了下一跳）。
3. 僵死无 close → 不重建 → 永久 000。

**方案**：
- `ps_teardown(p)`：`picoquic_discard_stream` 两侧 stream 连锁通知上下游回收，再 `ps_free`。
- `maybe_free` 改：本地 TCP 端 EOF(tcp_eof)且 q2t 空 → **主动 teardown 不等对端 FIN**（仅 ENTRY，见 B）。
- `flush_q2t` send 失败(EPIPE/ECONNRESET) → 丢弃 q2t + 标记 tcp_eof 触发回收。
- 空闲超时兜底 `IDLE_TIMEOUT_US=120s` 无数据活动强制 teardown。
- `ps_free` 幂等防 double-free；socks n<0 回收。

**验证**：coldtest 3轮×20 = 60/60，泄漏=0，middle Δ=60。

## B. 大流量截断根治

**现象**：修 A 后，loopback 2MB 只传 393KB 就截断，50MB 跨机 got=0。

**根因**：A 的主动 teardown 太激进。exit 从 target 秒读完响应全部 `add_to_stream` 排队到上游 stream(QUIC 实际发送受 cwnd 限，还在发)，target 一关 exit `tcp_eof`，而 exit 的 q2t 是去程 buffer(回程不占)→ `tcp_eof && q2t_len==0` 瞬间成立 → **discard 截断未发完的回程**。

**方案**：`maybe_free` 主动拆流**仅限 ENTRY**（client 关闭回程无意义可丢）；**EXIT 不主动 teardown**，走双向 FIN 让上游 stream 发完响应。

**验证**：loopback 50MB got=50000000 完整，157Mbps。

## C. P0-1 异步 DNS

**问题**：exit `tcp_connect_target` 的 `getaddrinfo` 同步阻塞事件循环，TikTok 几十域名 → 每个新域名解析冻结全局 → 直播规律卡顿。

**方案**：4 worker 线程 + 队列 + self-pipe（见架构 06）。`tcp_connect_target`→`tcp_connect_addr`(用已解析地址非阻塞connect)。

**验证**：exit 日志 `async DNS: 4 workers ready` + `resolving(async)` + `target connected`。

## D. P0-2 多连接池

**问题**：单 down_cnx 所有流共享一个 cwnd/pacing/flow-control。TikTok 爆发式并发排队；单点故障全死。

**方案**：`POOL_SIZE=6` 连接池，round-robin。`open_downstream_cnx`→`pool_ensure/pool_init/pool_pick`。close 回调从池移除惰性重建。

**验证**：entry/middle 各 `cnx pool ready 6/6`，coldtest 60/60。

## E. P0-3 流优先级 + 直播专用道

**问题**：直播(webcast/rtc)与视频下载竞争，bufferbloat 致直播延迟漂移。

**方案**：`classify_target` 按域名(webcast/pull/live/flv/rtmp/rtc)→prio。首部 `<prio>;` 端到端，三跳 `picoquic_set_stream_priority`。连接池前 `LAT_LANES=2` 条直播专用道。

**验证**：exit 日志 `livestream.com→prio=4`、`google→prio=20`。

## F. 窗口/Buffer

- `max_stream_data 1MiB→8MiB`：单流 26Mbps(1MiB/300ms三跳RTT限)→~200Mbps。
- `max_data 16→64MiB`；`max_ack_delay 25→5ms`。
- UDP `SO_RCVBUFFORCE 16MB`(实际32MB)：抗突发丢包。

**验证**：跨机干净单流 50MB 完整 26Mbps(窗口改前),改后理论~200Mbps。

## G. P1-2 批量收发

- **recvmmsg**：`udp_drain()` 一次收 32 包。
- **GSO**：`udp_send_batch()` 攒同目的地址+同段大小的包，`sendmsg + UDP_SEGMENT` 一次发，带逐段 sendto 回退。`addr_eq` 判目的。

**验证**：coldtest 60/60，QUIC 握手正常(对损坏敏感,通过=GSO正确)。

## H. 日志降级 + 链路质量观测

- 默认 `log4c_set_level(INFO)`，高频 per-stream 日志(socks CONNECT/resolving/target connected/nexthop)降 DEBUG，`NB_LOG_LEVEL=DEBUG` 可开。减单线程事件循环日志 I/O。
- **linkq**：entry/middle 每 10s `picoquic_get_default_path_quality` log `rtt/lost_pkt/cwin`。entry=gz→hk段,middle=hk→kz段。定位直播红黄+FEC输入。

## I. 白名单（访问控制）

**语义**：命中(域名后缀/IP CIDR/端口)走隧道，未命中拒绝。防无关流量占线路+安全。
**关键 bug 修复**：旧逻辑"只配域名时 IP 目标走 wl_ip_hit,n_ip=0 返回0拒绝"→ **TikTok 直播 IP 直连媒体流全被误拦**（直播红色断开根因!）。改为每维度独立空=不限制。修后直播绿色。
**运维**：exit `-W`，~5s 热重载。`deploy.py wl-push`(下发`tools/whitelist.local.conf`)/`wl-show`。

## 关键教训（务必记住）

1. **数据驱动诊断**：`nb_diag.py` 差量归因定位根因，不猜。
2. **手机干扰测试**：手机占满连接池，curl 测试挤不进；需隔离(coldtest)或用 exit 本地文件测吞吐。
3. **工具输出渲染故障**：Bash/Grep 内容输出常被污染(几千行重复)。**Read + 编译器 BUILD_OK 是真相**。远程 SSH 输出一般干净。
4. **编辑截断不执行**：消息被截断时工具不执行，别信编造的 success，每步验证。
