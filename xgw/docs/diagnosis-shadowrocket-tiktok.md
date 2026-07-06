# 诊断：Shadowrocket 连接不稳定 / TikTok 直播无法进行

> 现象：用 Shadowrocket 接入后，访问 `ip.sb` 能看到出口为**哈萨克斯坦**且“相对稳定”，
> 但整体连接**不稳定**，TikTok **直播基本无法进行**。
>
> 本文基于对实际代码（`xgw-edge/hy2-bridge`、`src/runtime.c`、`src/bridge.c`、
> `tools/deploy_fix.py`、`tmp/tmp-*.conf`）的审阅给出根因分析与修复建议，
> 按“最可能 → 次要”排序。
>
> **修复状态（2026-06-23 已实施代码改动，待线上验证）**：根因 A/B/C 与闲置首连超时均已在代码层修复，
> 见各节末「✅ 已修复」。生效需重新编译并按 egress→relay→ingress 重启，并用 UDP 出口验证。
>
> **补充修复（2026-06-24）**：egress UDP 语义从「单目标 connected UDP」改为「按 datagram 自带地址
> 收发的 unconnected UDP」，见 §1b。此前 `UDP_OPEN` 把 socket connect 到固定首目标、`UDP_DATA`
> 忽略帧自带地址只发首目标、回程也把来源写死成首目标，对一个 UDP socket 复用多目标（QUIC/直播常见）
> 会错发；现已修复。

---

## 0. 一句话结论

**`ip.sb` 走 TCP，能正确经 ingress→relay→egress 落地到 KZ；但 TikTok 直播严重依赖 UDP/QUIC，
而当前 HY2 前端对 UDP 的处理是「从入口机本地直接出网」，根本没有进入 xgw 的 relay/egress 链路。**
于是 UDP 流量出口 IP 不是 KZ（而是广州入口机的中国 IP），TikTok 直接判定异常 → 直播失败；
同时 TCP 与 UDP 出口不一致、跨境段无 PMTUD，叠加成“整体不稳定”。

---

## 1. 根因 A（决定性）：HY2 前端 UDP 不走 xgw 链路

证据：`xgw-edge/hy2-bridge/main.go`

```go
// TCP：经 bridge_tcp → xgw ingress → relay → egress（正确）
func (o *bridgeOutbound) TCP(reqAddr string) (net.Conn, error) {
    ...
    conn, err := net.DialTimeout("tcp", bridgeTCPAddr, 10*time.Second) // 进 xgw bridge
    writeBridgeRequest(conn, reqAddr)
    readBridgeStatus(conn)
    ...
}

// UDP：直接在入口机本地开 socket 出网（错误！没有 bridge，没有 relay/egress）
func (o *bridgeOutbound) UDP(reqAddr string) (coreServer.UDPConn, error) {
    conn, err := net.ListenUDP("udp", nil)   // ← 直接本机出网
    ...
}
```

**含义**：

- `curl http://ip.sb`（HTTP/TCP）→ 走 `TCP()` → 经 xgw 三跳 → 出口 = `2.135.147.71`（KZ）。
  这正是旧 `hy2bridge_deploy.py verify` 唯一验证的路径，所以“ip.sb 出口 KZ、相对稳定”。
- TikTok 直播的拉流/推流、QUIC、以及大量 UDP 业务 → 走 `UDP()` → **从广州入口机直接出网**。
  - 出口 IP 是中国 IP（与 TCP 看到的 KZ 完全不同）。
  - TikTok 对“同一会话 TCP 在 KZ、UDP 在 CN”这种地理/网络不一致极其敏感，会限制或拒绝直播。
  - 中国直出 UDP 到 TikTok 边缘，丢包/QoS 本身就差，直播更难成立。

> 注意：xgw 的 `bridge.c` 帧层其实**支持 UDP**（`XGW_BRIDGE_KIND_UDP_OPEN/DATA/CLOSE`）。
> egress 侧会按 datagram 自带地址用 `sendto` 落地（见 §1b 的语义修复，早期版本是 connected UDP）。
> 缺的是**前端没把 UDP 交给 bridge**——是 `hy2-bridge` 这一层把 UDP“短路”了。

### 修复 A（首要）

让前端 UDP 也经 xgw bridge 落地，而不是本机直出。两种做法：

1. **改 `bridgeOutbound.UDP`**：参照 `TCP()`，对 UDP 也走 bridge 协议
   （发 `UDP_OPEN`/`UDP_DATA`，由 egress 按 datagram 自带地址 `sendto` 落地，见 §1b）。
   bridge ingress/egress 的 UDP 帧已具备，需要把 `hy2-bridge` 的 `coreServer.UDPConn`
   实现接到 bridge 通道，而非 `net.ListenUDP`。
2. **退一步先验证**：临时在 egress 机上确认 UDP 链路确实通（`xgw outbound-smoke <cfg> udp host:port`），
   再把前端接上去。

修复后务必用 **UDP/QUIC 出口**重新验证（现有 verify 只测 TCP，见 §6）。

> ✅ **已修复（2026-06-23）**：`xgw-edge/hy2-bridge/main.go` 已让 `bridgeOutbound.UDP` 在
> `bridgeTCPAddr != ""` 时经 bridge TCP 落地：新增 `xgwBridgeUDPConn`（实现 hysteria
> `UDPConn` 接口）与 `writeBridgeUDPRequest`，按 bridge UDP 前端帧
> `addr_len|addr|data_len|data` 收发，UDP 真实出口随 TCP 一同走 ingress→relay→egress(KZ)。
> `bridgeTCPAddr == ""` 时保留本机直连兜底（仅开发）。启动须带 `--bridge-tcp 127.0.0.1:19080`。

---

## 1b. 根因 A2：egress UDP 单目标 connected 语义（多目标错发）

证据（修复前 `src/bridge.c`）：

```c
// UDP_OPEN：把 socket connect 到固定首目标
session->fd = session->is_udp ? udp_connect_ipv4(host, port) : tcp_connect_ipv4(host, port);

// UDP_DATA：解析出帧自带 addr 却丢弃，send() 到 connected fd（永远发首目标）
int sent = send(session->fd, payload + 13U + addr_len, data_len, 0);

// 入站回程：把 session->target_host（首目标）当来源地址，而非真实来源
egress_queue_push_ex(session, session->is_udp ? session->target_host : NULL, read_buf, nread);

// 回程编码：if/else 两支恒等死分支，read_addr（真实来源）从不使用
if (read_addr[0] != '\0') {
    snprintf(addr, ..., "%s:%u", session->target_host, session->target_port);
} else {
    snprintf(addr, ..., "%s:%u", session->target_host, session->target_port);
}
```

**含义**：Go 客户端的 UDP 前端帧本就携带 per-datagram 目标地址（`addr_len|addr|data_len|data`，
`SendDatagramTo` / `encodeDatagram`），但 egress 落地时把它丢弃，只往 `UDP_OPEN` 的首目标发送。
对**单目标** UDP 流（单边缘节点）没问题；但 QUIC / 直播常把**一个 UDP socket 复用到多个目标**
（多个 CDN 边缘、ICE/STUN 多候选），这些 datagram 会被全部错发到首目标，回程来源也被写死成首目标——
表现为 UDP「通了但行为诡异 / 部分目标不可达」，与 §1 的「UDP 根本不进链路」是两个不同层次的问题。

### 修复 A2

> ✅ **已修复（2026-06-24）**：egress UDP 改为「按 datagram 自带地址收发的 unconnected UDP」。
> - `udp_open_unbound()` 取代 `udp_connect_ipv4()`：建 unconnected `SOCK_DGRAM`，首目标仅用于校验可解析 +
>   决定 socket family，**不 connect**。
> - `UDP_DATA` 出站改 `sendto()`：解析帧自带文本地址（`host:port` / `[v6]:port`）→ `bridge_resolve_endpoint_cached`
>   → 发往该地址，不再发固定首目标。
> - 入站 `bridge_egress_drain_socket` 改 `recvfrom()`：取真实来源 sockaddr，经新 helper `bridge_sockaddr_to_text`
>   格式化为 `host:port` 写入回程地址。
> - `xgw_bridge_egress_dequeue_ex` 消除 if/else 死分支：用 `read_addr`（真实来源）编码回程，仅缺失时回退首目标。
> - **UDP 容错语义修正**：单个 datagram 解析/`sendto` 失败只丢弃并记日志，不再 `pending_close` 杀整条会话。
> - **平台兼容**：unconnected UDP 失去 ICMP→recv 错误语义，Windows `WSAECONNRESET`(10054) / Linux
>   `ECONNREFUSED` 在 UDP 下被吞掉不关流；建 socket 时关闭 `SIO_UDP_CONNRESET`；UDP 0 长 datagram 不当 EOF。
>
> **已知限制（backlog）**：unbound socket 的 family 由首目标决定，混族（v4↔v6）stream 出站会丢包但不崩溃；
> 失去 ICMP 自动关流后，死 UDP 会话依赖 `TCP_CLOSE` / 回程队列满清理（后续可加基于 `last_activity_us`
> 的 idle sweep）。
>
> 验证：用 `xgw-native-probe` / `xgw-sim-client` 走一条 UDP stream 向**两个不同目标**发包，
> egress 日志 `bridge.egress.send proto=udp target=…` 的 target 应随帧地址变化（而非恒为首目标），
> 回程帧 addr 应等于真实来源。

---

## 2. 根因 B：MTU / 分片，跨境段易丢包

证据：
- profile 固定 `mtu=1380`、`payload=1200`（`protocol.c:56-57`、`live-bbr.conf:15-16`）。
- 加封固定 +24 字节（nonce 8 + tag 16，`security.c`）。
- `transport_udp.c` **未设置** `IP_DONTFRAG`/`IP_MTU_DISCOVER`，**无 PMTUD**。
- 外层还要套 HY2/QUIC + UDP/IP 头。

**含义**：手机→广州、香港→海外两段是抖动主来源（`protocol.md` 自己也这么说）。
当 `1380 + 24 + 外层封装` 超过路径真实 PMTU（跨境/移动网络常见 1400 以下）时，
UDP 包要么被中间设备分片（更易丢、重组失败），要么直接被丢。
TCP（ip.sb）有 MSS 钳制和内核分段，受影响小；UDP/QUIC 直播受影响大。

### 修复 B

- 把承载 payload 下调，给外层封装留足空间。先试 `mtu=1280`、`payload=1100` 左右
  （部署配置 `tmp/tmp-*.conf` 当前**没写** MTU/payload，取 profile 默认 1380/1200，需显式收紧）。
- 中长期：在 `transport_udp.c` 接入 PMTUD 或固定一个保守 effective MTU。

> ✅ **已修复（2026-06-23）**：`src/protocol.c` profile 默认改为 `mtu=1280`/`payload_size=1100`；
> 当前由 `tools/deploy_fix.py` 统一生成并下发的三份 `tmp/tmp-*.conf` / `configs/*.conf` 已显式写
> `mtu=1280`/`payload_size=1100`；`sample/live-bbr.conf` 同步。selftest 通过（payload 1100 分片/FEC 正常）。

---

## 3. 根因 C：FEC / 拥塞参数偏弱，直播抗抖动不足

证据：
- 线上 `tmp/tmp-*.conf` 用 `profile=live-bbr` 且**未覆盖** FEC，取默认 `4 data + 2 parity`、
  `pacing 50 Mbps`、`redundant_copies=1`。
- `hy2-bridge` 设了 `IgnoreClientBandwidth: true`（`main.go:289`），
  即**忽略客户端带宽协商**，Brutal 那种“客户端拉高速率”的能力被关掉。

**含义**：直播是持续高码率 + 突发丢包场景。50 Mbps pacing + 单副本 + 2 校验，
在跨境抖动下恢复能力一般；而 `IgnoreClientBandwidth` 让客户端无法把速率/激进度顶上去。

### 修复 C

- 直播线路试用 `profile=live-brutal`（`6+3` FEC、`200 Mbps`、`redundant_copies=2`），
  或在 `live-bbr` 配置里显式调高 `fec_parity_shards`、`redundant_copies`。
- 评估是否关闭 `IgnoreClientBandwidth`，让带宽协商生效（需确认与官方 HY2 客户端兼容）。
- 运行时 `stream_sched` 已会按丢包自适应加 parity，但前提是 UDP 真的走进了链路（先解 §1）。

> ✅ **已定调（2026-06-23）**：实际 HY2 部署（广州-香港HY2-美国HY2）用 BBR 已较稳，
> 故**线上继续用 `profile=live-bbr`，不切 brutal、不强行提高 FEC**。同时按需求精简了拥塞模式：
> 代码层**只保留 BBR 与 Brutal**，删除了 RENO、NONE 拥塞模式、BBR 的 conservative/aggressive
> 子档与 live-none profile（`xgw_protocol.h` 枚举、`protocol.c` 解析/命名、`cc.c` gains/reno、
> `config.c`、`smoke_orchestrator.py`）。旧配置里的 `reno`/`live-none`/`aggressive` 会**回退到
> bbr/live-bbr/standard 并告警**，不会导致启动失败。`IgnoreClientBandwidth` 暂保持（官方 core 默认行为）。

---

## 4. 根因 D：Shadowrocket ↔ 兼容前端的对齐缺口

证据：`xgw-edge/docs/hy2-alignment-checklist.md` 与 `examples/shadowrocket-hy2-template.md`
明确写着“**仍待对齐**”：

- “更完整的 UDP 分片 / 重组行为”尚未完成；
- “Shadowrocket 真机是否 100% 接受当前兼容实现”**未确认**；
- Salamander 混淆互通**未确认**。

注意生产前端 `hy2-bridge/main.go` 直接用了 **Hysteria2 官方 core**（`coreServer`），
理论上协议兼容性较好；但 `internal/proxy/hy2_udp.go` 那套**自研兼容实现**的 UDP 分片
仍是半成品。要确认 Shadowrocket 实际连的是哪一个二进制：

- 若连的是 `hy2-bridge`（官方 core）→ 协议层 OK，问题集中在 §1（UDP 直出）。
- 若连的是 `xgw-edge-server`（自研兼容）→ 再叠加 UDP 分片/session 未对齐的风险。

### 修复 D

- 确认线上前端进程与端口（`1023` vs `8443`）、对应二进制。
- Shadowrocket 侧按模板核对：`类型=Hysteria 2`、`ALPN=h3`、`UDP=开启`、
  SNI 与证书一致、token 一致；测试期可勾“跳过证书验证”。
- 若用 Salamander，前端必须配同样的 `obfs`/`obfs-password`，否则握手层就不稳。

---

## 5. 根因 E：会话/重启时序导致的偶发不稳

证据：`hy2bridge-deploy.md:54-63` 与当前部署脚本重启顺序——
必须按 **egress → relay → ingress** 重启，否则 ingress 残留旧“已建立”安全状态会触发 `secure_open_fail`。
另：keepalive 默认 10s（`tmp/tmp-*.conf`），移动网络 NAT 表项可能更短。

### 修复 E

- 重启严格走 `egress→relay→ingress`（用现成脚本，别手工单独重启 ingress）。
- 移动端不稳可把 `keepalive_sec` 调小（如 5s）抵抗 NAT 超时。
- 出现 `secure_open_fail`/`channel.select.not_ready` 日志时，按时序整体重启。

---

## 5b. 根因 F：长闲置（~10 分钟）后 Shadowrocket 首次测试连接超时

证据与机理：
- 客户端↔前端的 **QUIC 连接空闲上限默认 30s**（`server/config.go defaultMaxIdleTimeout`），
  前端 **UDP session 空闲上限默认 60s**（`main.go UDPIdleTimeout`）。
- 后端三跳因 keepalive（3-10s/slot）持续保活通常不断；但客户端↔前端这条 QUIC 在长闲置后被回收。
- 重新测试时，Shadowrocket 需重建 QUIC + 重认证，若恰逢后端 `maybe_refresh_control_session`
  在 idle 阈值（原默认 30s-2s）触发把控制会话**硬重置到 INIT**，首包落在重握手窗口被丢 → 首拨超时；
  第二拨时链路已就绪 → 成功。浏览器访问 ip.sb 正常，是因为它发起的是**新的流**，不依赖被回收的旧连接。

### 修复 F

> ✅ **已修复（2026-06-23，前端为主 + 后端兜底）**：
> - 前端 `xgw-edge/hy2-bridge/main.go`：`QUICConfig.MaxIdleTimeout` 从默认 30s 提到**最大允许的 120s**，
>   `UDPIdleTimeout` 从 60s 提到 **300s**，使客户端在闲置期内的 keepalive 足以维持连接、不被回收。
> - 后端 `src/config.c`：`max_idle_timeout_sec` 默认从 30s 提到 **120s**（对齐前端 QUIC），
>   `maybe_refresh_control_session` 的硬重置仅作为最后手段（keepalive 正常时不会触发）。
> - 受 hysteria core 约束 `MaxIdleTimeout ≤ 120s`，无法再高；如仍需更长闲置存活，
>   建议客户端开启 keepalive（Shadowrocket HY2 配置里设 keepalive），由客户端主动保活。

---

## 6. 验证方法（必须覆盖 UDP，而不只是 ip.sb）

当前旧 `tools/hy2bridge_deploy.py verify` 只做 `curl --proxy http http://ip.sb/`，**纯 TCP**，
所以它“成功”并不能代表直播可用。补充验证：

1. **出口一致性**：分别测 TCP 与 UDP 出口 IP，确认 **UDP 出口也是 `2.135.147.71`**。
   - TCP：`curl --proxy http://127.0.0.1:<port> http://ip.sb`
   - UDP/QUIC：用支持 QUIC 的方式测（如 `curl --http3 https://...`，或抓包看 UDP 实际从哪出）。
2. **egress 日志**应出现 `bridge.egress.send … proto=udp target=…`（说明 UDP 真落地到 KZ）。
   现在很可能**只看得到 TCP**，看不到 UDP——这就坐实了 §1。
   多目标场景下，`target=` 应**随 datagram 自带地址变化**（验证 §1b 的 per-datagram sendto 修复），
   而非恒为 `UDP_OPEN` 的首目标。
3. **relay 日志**：UDP 流量也应出现 `runtime.forward.select`（目前只有 TCP 出现）。
4. TikTok 实测：先确认能访问 `live.tiktok.com`（白名单已含），再测拉流/推流。

---

## 7. 处置优先级建议

| 优先级 | 动作 | 对应根因 |
|--------|------|----------|
| P0 | 让前端 UDP 走 xgw bridge（不再本机直出），并用 UDP 出口重新验证 | §1 A |
| P0 | egress UDP 按 datagram 自带地址 sendto/recvfrom（不再单目标 connected）✅ 已修复 2026-06-24 | §1b A2 |
| P0 | 显式收紧 MTU/payload（如 1280/1100），给外层封装留空间 | §2 B |
| P1 | 直播线路切 `live-brutal` 或提高 FEC/副本；评估关 `IgnoreClientBandwidth` | §3 C |
| P1 | 确认 Shadowrocket 连的前端二进制/端口/SNI/obfs 完全对齐 | §4 D |
| P2 | 严守 egress→relay→ingress 重启时序；调小 keepalive | §5 E |
| P2 | 统一部署目录与脚本入口，避免旧文档/旧脚本名继续误导运维 | 运维 |

> 核心判断：**TCP 通、UDP 不通到 KZ** 是当前一切现象的主线。先把 UDP 拉进 xgw 链路（P0），
> TikTok 直播才有成立的前提；MTU/FEC 是把“能用”变“稳定”的后续优化。
</content>
