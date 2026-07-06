# NB 架构设计文档

> [01-需求](01-requirements.md) | 02-架构(本篇) | [03-已实现](03-implementation.md) | [04-部署运维](04-deployment-ops.md) | [05-路线图](05-roadmap.md)

## 1. 总体架构

一份二进制 `nb_node`，`-r` 选角色，组成 **entry → middle(×N) → exit** 可变跳数链路。

```
手机App → Shadowrocket(SOCKS5) → entry(gz广州) → [QUIC] → middle(hk香港) → [QUIC] → exit(kz哈萨克) → target
                                    :1080 SOCKS        UDP4443             UDP4443
```

| 角色 | left(靠client) | right(靠target) | 职责 |
|---|---|---|---|
| **entry** | 本地 TCP accept(SOCKS5) | 下游 QUIC(第一跳) | 每 TCP 连接开一个 QUIC bidi stream，首部发 source-route，TCP↔QUIC双向泵 |
| **middle** | 上游 QUIC(上一跳) | 下游 QUIC(下一跳) | 读首部 `H:host:port` 动态连下一跳，转发剩余 route+数据。**无状态** |
| **exit** | 上游 QUIC(上一跳) | 目标 TCP | 读首部 `T:host:port`，connect 目标，QUIC↔TCP双向泵 |

## 2. source-route 协议

stream 首部第一行（文本 + `\n`），语义 = 接收节点出发的剩余路径：
```
<prio>;H:hop2:qport,...,T:targethost:targetport
```
- `<prio>;` 前缀 = 流优先级（PRIO_LATENCY=4 直播 / PRIO_BULK=20 批量），端到端传递，各跳解析后 `set_stream_priority`。
- `H:` = 下一 QUIC 跳地址（middle 消费第一个 H，转发其余）。
- `T:` = 最终 TCP 目标（exit 消费）。
- 两跳（entry 直连 exit）时 route 仅 `T:target:port`（无 H）。
- 例(三跳)：entry 发 `20;H:2.135.147.102:4443,T:www.example.com:443`

## 3. 统一数据模型 `proxy_stream_t`

每条被代理的流，left(靠client) ↔ right(靠target) 双向缓冲/配对。关键字段：
- `up_cnx/up_stream_id`：上游 QUIC(middle/exit 收上一跳的 server stream)
- `down_cnx/down_stream_id/down_opened`：下游 QUIC(entry/middle 到下一跳的 client stream)
- `tcp_fd/tcp_connecting`：entry=客户端conn / exit=目标conn
- `hdr/hdr_done/hdr_len`：首部解析(middle/exit)
- `q2t/q2t_len/q2t_cap/q2t_fin`：面向 TCP 的待写缓冲(动态增长)。entry写回客户端 / exit写目标
- `tcp_eof/up_fin_seen/down_fin_seen`：FIN 追踪(去程left→right,回程right→left)
- `last_active`：空闲超时兜底
- `dns_pending/target_port`：exit 异步 DNS 中
- `prio`：流优先级
- `socks_stage/socks_buf/socks_len`：entry SOCKS5 握手状态机

回调按 `(cnx,stream_id)` 双向查找配对：`ps_find_by_up` / `ps_find_by_down`。去程 `fwd_down`/回程 `fwd_up` 统一转发。`MAX_CONN=1024` 流表。

## 4. 自定义事件循环（单进程单线程）

`main()` 的 `for(;;)`：
```
1. 构建 fd_set(udp_fd + DNS.pipe_rd + tcp_listen_fd + 各流 tcp_fd)
2. select(timeout = picoquic_get_next_wake_delay, cap 1s)
3. UDP→QUIC: udp_drain()  [recvmmsg 批量收 → picoquic_incoming_packet]
3.5 DNS完成: DNS.pipe_rd可读 → dns_pop_result → tcp_connect_addr 非阻塞connect(exit)
4. TCP accept(entry): entry_accept()
5. 每流TCP IO: connect完成检测 / socks_handshake 或 pump_tcp / flush_q2t / maybe_free
6. middle无TCP流判释放 + 空闲超时兜底扫描
7. 白名单热重载(~5s) + 链路质量观测(~10s)
8. QUIC→UDP: udp_send_batch()  [picoquic_prepare_next_packet → GSO sendmsg 批量发]
```

**关键约束**：picoquic 的 quic context 非线程安全，一个 context 一个线程驱动。多核扩展需每线程独立 context + SO_REUSEPORT（见 05）。

## 5. 连接池 `cnx_pool_t`（P0-2）

entry/middle 到下一跳维护 `POOL_SIZE=6` 条并行 QUIC 连接，每条独立 cwin/pacing/flow-control。
- `cnx[POOL_SIZE]`：连接(NULL=空槽待建/已关)
- `next_sid[POOL_SIZE]`：每条独立的 client bidi stream id 分配(0,4,8..)
- 分区：前 `LAT_LANES=2` 条为**直播专用道**(round-robin, `rr_lat`)；后 4 条批量道(`rr`)。`pool_pick(pool, is_latency)`。
- `pool_ensure` 惰性建/重建空槽；`pool_init` 建满；close 回调置 NULL 下次惰性重建。

## 6. 异步 DNS 子系统（P0-1）

exit 的 `getaddrinfo` 阻塞，移出事件循环：
- `DNS_WORKERS=4` worker 线程 + 请求/完成环形队列(`DNS_QCAP=4096`) + mutex/cond + self-pipe。
- exit on_up_data 的 T: 分支 `dns_submit(ps_id,host,port)` → worker getaddrinfo → 完成队列 + 写 self-pipe → 主循环 select 唤醒 → `dns_pop_result` → `tcp_connect_addr` 非阻塞 connect。
- 用 `ps_id`(流水号)关联,不跨线程传指针,防解析期间流释放的 use-after-free。

## 7. 白名单子系统（访问控制）

- 全局 `WL` 结构：domain[后缀]/ip[CIDR]/port 数组 + enabled + path + mtime。
- `whitelist_allowed(host,port)`：**每维度独立,空=不限制**。port必命中；host 是 IP 走 CIDR(n_ip==0则放行)，否则走域名后缀(n_dom==0则放行)。
- 集成：entry socks_handshake + exit on_up_data 的 T: 分支，未命中回拒绝/reset。
- `-W <file>` 加载，事件循环 ~5s 检查 mtime 热重载。日志 `linkq`/`BLOCKED`(WARN)。

## 8. 传输参数（picoquic）

- 拥塞：`picoquic_bbr_algorithm`(bbr.c = BBRv3)
- `initial_max_streams_bidi=2000`
- `initial_max_stream_data_bidi_local/remote=8MiB`(单流吞吐≈窗口/RTT,三跳~300ms下1MiB限26Mbps,8MiB→~200Mbps)
- `initial_max_data=64MiB`(连接级,支撑多流)
- `max_ack_delay=5ms`(默认25ms,更快ACK降RTT)
- UDP socket `SO_RCVBUFFORCE/SO_SNDBUFFORCE=16MB`(实际32MB,绕过rmem_max)

## 9. 关键设计决策

1. **中间节点无状态**：路径由 entry 下发 route 决定 → 适配卖线路/动态选路。
2. **多连接池而非单连接**：消除 cwnd/pacing 共享瓶颈，容量叠加，单连接故障不影响全局。
3. **流优先级+专用道**：直播与批量物理隔离(不同连接)+ QUIC stream 优先级,避免 bufferbloat。
4. **异步 DNS**：单线程事件循环不能被 getaddrinfo 阻塞。
5. **q2t 永不丢数据**：动态增长,背压靠 QUIC connection flow control。
