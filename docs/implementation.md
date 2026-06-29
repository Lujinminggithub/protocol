# XGW 详细实现说明

> 本文档面向开发与运维，系统性描述 `tools/xgw` 的数据面实现、模块职责、线上协议、
> 端到端数据通路，以及当前真实直播链路（HY2 入口 + xgw 传输面）的拼装方式。
> 与 `docs/protocol.md`（设计意图）互补：本文聚焦“代码里实际怎么实现的”。

---

## 1. 系统总览

xgw 是一个用 C 实现的、面向**固定多跳直播链路**的 UDP 传输数据面。整条业务链路是：

```
Shadowrocket / 指纹浏览器
        │  (Hysteria2 / QUIC, TLS1.3, ALPN=h3)
        ▼
  HY2 前端 (xgw-edge/hy2-bridge, :1023)        ← QUIC 控制面 + 认证
        │  TCP: 经 bridge_tcp → xgw ingress
        │  UDP: 见 §9 「已知缺陷」
        ▼
  xgw ingress (广州, role=ingress, :51840)     ← 入口抗抖动 / 分片 / FEC
        │  专线段（稳定，走内网 IP）
        ▼
  xgw relay   (香港, role=relay, :51840)        ← 纯转发，不落地
        │  跨境段（主要抖动来源）
        ▼
  xgw egress  (哈萨克斯坦, role=egress, :51840) ← 出口落地，bridge_egress 建真实连接
        ▼
  目标服务 (TikTok 直播 / ip.sb / CDN)
```

- **控制面**用 Go（Hysteria2 官方 core）：认证、QUIC、伪装。
- **数据面**用 C（本目录）：pacing、FEC、session 表、调度、零拷贝 IO。
- 路径在配置里**显式固定**（`path=...` 一行），没有动态路由发现。

### 1.1 角色模型

| 角色 | 配置 `role=` | 职责 | 是否落地真实连接 |
|------|-------------|------|------------------|
| mobile | （客户端） | 发送端 | — |
| ingress | `ingress` | 广州入口，接 HY2 bridge，分片/FEC/调度 | 否（转发给 relay） |
| relay | `relay` | 香港中继，纯转发（`runtime.forward.select`） | 否 |
| egress | `egress` | 海外出口，`bridge_egress` 建立到目标的 TCP/UDP | 是 |

---

## 2. 源码模块地图

编译单元见 `Makefile`。按职责分组：

### 协议与帧
- `src/protocol.c` / `include/xgw_protocol.h` — 帧头编解码、profile 模型、握手 payload。
- `src/frame.c` / `include/xgw_frame.h` — 分片、Reed-Solomon GF(256) FEC、重组。
- `src/crypto.c` / `include/xgw_crypto.h` — 纯 C SHA-256、HMAC、hash-expand 流密码。
- `src/security.c` / `include/xgw_security.h` — 每会话密钥派生、payload 加封/解封。

### 会话与拥塞
- `src/session.c` / `include/xgw_session.h` — session 表、replay window、channel/slot 多路复用。
- `src/cc.c` / `include/xgw_cc.h` — BBR（3 profile）、Brutal、Reno、pacing。
- `src/control.c` / `include/xgw_control.h` — 握手状态机（HELLO/HELLO_ACK/CONFIRM）、ACK。

### 数据面与运行时
- `src/dataplane.c` / `include/xgw_dataplane.h` — 收包处理 + 发包拼装的核心。
- `src/runtime.c` / `include/xgw_runtime.h` — 主循环，串接 TUN/UDP/bridge/调度/keepalive。
- `src/route.c` / `include/xgw_route.h` — 多线路（active/candidate/draining）状态机。
- `src/bridge.c` / `include/xgw_bridge.h` — bridge ingress（接 HY2）与 bridge egress（出口落地）。

### 传输与平台
- `src/transport_udp.c` / `include/xgw_transport.h` — UDP socket 抽象（含 Windows Winsock）。
- `src/tun_linux.c` / `src/tun_stub.c` / `include/xgw_tun.h` — Linux TUN。
- `src/afxdp_linux.c` / `src/afxdp_stub.c` / `include/xgw_afxdp.h` — AF_XDP 零拷贝（骨架）。
- `bpf/xdp_tunnel_kern.c` — XDP/XSK 重定向 BPF 程序。

### 策略、出站、运维
- `src/policy.c` / `include/xgw_policy.h` — 白名单 CIDR/域名、动态学习 IP、DOS 防护。
- `src/acl.c` / `include/xgw_acl.h` — 目标匹配到 outbound。
- `src/outbound.c` / `include/xgw_outbound.h` — DIRECT/FIXED/SOCKS5/HTTP 出站。
- `src/pool.c` / `include/xgw_pool.h` — 出口节点池选择。
- `src/obfs.c` / `include/xgw_obfs.h` — Salamander 风格混淆层（可选）。
- `src/tuning.c` / `include/xgw_tuning.h` — 缓冲区、限速、调试开关。
- `src/config.c` / `include/xgw_config.h` — `key=value` 配置解析。
- `src/main.c` — CLI 入口（`run` / `selftest` / `probe-send` / `outbound-smoke` 等）。
- `src/singbox_sidecar.c` — sing-box sidecar 集成点。

---

## 3. 线上协议（on-wire）

### 3.1 主帧头（`XGW_HEADER_SIZE = 20` 字节，大端）

| 偏移 | 字段 | 类型 | 说明 |
|------|------|------|------|
| 0 | version | u8 | 协议版本 |
| 1 | type | u8 | 消息类型（见下） |
| 2-3 | flags | u16 | `SECURE(0x1)` / `ACK_ELICITING(0x2)` / `CONTROL(0x4)` |
| 4-7 | session_id | u32 | 会话 ID |
| 8-15 | sequence | u64 | 每会话序号（RTT/replay） |
| 16-17 | payload_length | u16 | 载荷长度 |
| 18-19 | reserved | u16 | 保留 |

**消息类型**：`HELLO(1)` `HELLO_ACK(2)` `CONFIRM(3)` `DATA(4)` `KEEPALIVE(5)` `FEC(6)` `ACK(7)`。
编解码见 `protocol.c`（`xgw_header_encode` / `xgw_header_decode`、`write_be32`/`read_be32`）。

### 3.2 分片头（`XGW_FRAGMENT_HEADER_SIZE = 10`）

`frame.c:14-23`：`group_id(u32) | index(u16) | count(u16) | payload_len(u16)`。
一个大包被切成 ≤ `payload_size` 的多帧，同一 `group_id` 内做 FEC。

### 3.3 FEC 头（`XGW_FEC_HEADER_SIZE = 12`）

`group_id(u32) | index(u8) | data_count(u8) | parity_count(u8) | scheme(u8) | payload_len(u16) | reserved(u16)`。
RS(GF256) 校验块通过矩阵乘法生成；接收侧凑齐任意 `data_count` 个分片即可解码。

### 3.4 加封后的 DATA payload（`security.c`）

```
[ nonce(8) ] [ ciphertext = plaintext XOR hash_expand(tx_key, nonce) ] [ HMAC-SHA256(tx_key,…)[:16] ]
```

每帧固定增加 **8（nonce）+ 16（tag）= 24 字节** 开销。

### 3.5 bridge 内层协议（`XGB1`）

bridge ingress/egress 之间的应用层载荷用 `0x58474231`（"XGB1"）魔数封装：

```
magic(4)="XGB1" | stream_id(u32) | kind(u8) | …
kind: 1=TCP_OPEN 2=TCP_DATA 3=TCP_CLOSE 4=UDP_OPEN 5=UDP_DATA 6=UDP_CLOSE
```

解析见 `runtime.c:parse_bridge_payload_meta`；egress 侧消费见 `bridge.c`（`bridge_egress_handle_*`）。
**TCP 与 UDP 都在帧层支持**，但 UDP 实际是否走到这里取决于前端是否发 `UDP_OPEN`（见 §9）。

---

## 4. 加密与密钥

- 原语全部纯 C（`crypto.c`），无外部依赖：SHA-256、HMAC-SHA256、`xgw_hash_expand_xor`（HKDF 式扩展异或）。
- 预共享 `auth_token` 派生静态密钥 `static_key`；握手交换 nonce 后派生方向密钥：

```
initiator:  tx = HMAC(static_key, "tx" || local_nonce || peer_nonce)
            rx = HMAC(static_key, "rx" || peer_nonce  || local_nonce)
responder:  tx/rx 互换
```

- **无前向保密**（PSK 对称派生），定位为受管理的固定链路，不是公网开放代理。
- 控制帧 auth_tag 为 HMAC 截断 16 字节，覆盖固定前缀字段。

---

## 5. 会话、多路复用与 replay

`session.h` 关键结构：

- `xgw_session_t`：`id`、`remote_addr`、`next_tx_seq`/`next_rx_seq`、`replay_window`、
  `control_state`、`security`、`cc`、`outstanding[512]`、各类时间戳。最大 `XGW_MAX_SESSIONS=256`。
- **Replay window**：256-bit（`bitmap[4]`），`max_seq` 之外即拒；窗口内查 bitmap。
  reorder 容忍由 profile 的 `reorder_window`（默认 128）决定。
- **Channel/slot 多路复用**：每个邻居方向（`PREVIOUS`/`NEXT`）有 3 个 slot：
  `CONTROL(0)` / `MEDIA(1)` / `BULK(2)`，由 `xgw_flow_class_to_slot()` 按流量类映射。
- `get_send_session()`（`runtime.c:1833`）用 `line_id|seg|slot` 做稳定哈希生成 `session_id`，
  保证同一段链路、同一 slot 双向一致。

---

## 6. 拥塞控制（`cc.c`）

| 模式 | 说明 |
|------|------|
| BBR | 4 状态机 `STARTUP/DRAIN/PROBE_BW/PROBE_RTT`；当前只保留 standard 一档增益（gain 2.2/1.25/0.75/2.0） |
| Brutal | 固定目标带宽，`pacing_rate = max(target, sample_bw)`，激进、几乎不退避 |

> 2026-06-23 起按需求**只保留 BBR 与 Brutal** 两种拥塞模式；已删除 RENO、NONE，以及
> BBR 的 conservative/aggressive 子档。配置里的历史值（`reno`/`none`/`conservative`/`aggressive`）
> 解析时**回退到 bbr/standard 并告警**，不报错。

- BDP：`bandwidth_bps/8 * max(rtt_us,1000) / 1e6`。
- pacing 延迟：`delay_ns = bytes*8*1e9 / pacing_rate_bps`。
- 发送门控：`inflight + frame <= cwnd` 且 `now >= next_send_ns`。

---

## 7. Profile 模型（`protocol.c:53-83`）

| profile | mtu | payload | fec_data | fec_parity | pacing_rate | redundant |
|---------|-----|---------|----------|-----------|-------------|-----------|
| live-bbr | 1280 | 1100 | 4 | 2 | 50 Mbps | 1 |
| live-brutal | 1280 | 1100 | 6 | 3 | 200 Mbps | 2 |

> 2026-06-23 起：profile 默认 MTU/payload 从 1380/1200 收紧为 **1280/1100**（给外层 QUIC/封装留
> PMTU 余量）；并删除了 `live-none`（历史值回退 live-bbr 并告警）。线上部署配置（生成器 +
> `tmp/tmp-*.conf`）也已显式写 `mtu=1280`/`payload_size=1100`。

运行时 parity 还会被 `runtime.c` 的 `stream_sched_*` 按实测丢包/恢复率动态加减
（`parity_budget`，上限 `XGW_MAX_FEC_PARITY`）。

---

## 8. 端到端数据通路

### 8.1 收包（`dataplane.c:xgw_dataplane_process_frame`）

```
UDP recv → DOS/ACL 门控 → header_decode → session_upsert → 必要时 security_init
  ├─ 控制帧 → control_process（验 auth_tag、推进状态机、必要时派生密钥、回 HELLO_ACK/CONFIRM）
  └─ 数据帧 → 解密(若 SECURE) → replay 检查 → note_rx
            → fec_add_data / fec_add_parity → 凑齐则 reassemble
            → 标记 pending_ack → 视情况立即/按节奏回 ACK
返回 xgw_reassembly_result_t { packet, control_frame, session_id, recovered }
```

### 8.2 发包（`dataplane.c:xgw_dataplane_build_outbound`）

```
payload(≤payload_size) → 分片(build_data_frames)
  → 选 parity 数(按 flow_hint / cc 状态 / inflight) → build_fec_frames(RS 编码)
  → 每帧 security_seal_payload(加 nonce+tag)
  → session_record_send（RTT/loss 追踪）
返回 batch { frames[], frame_lengths[], data/fec count, group_id, last_sequence }
```

### 8.3 转发（relay，`runtime.c:select_forward_target`）

relay 不落地：根据来包源地址匹配 `hops->previous`/`hops->next`，
打印 `runtime.forward.select from=… to=…`，把帧转给另一侧。
线上成功判据之一就是 relay 日志里出现
`runtime.forward.select from=next to=previous` + `runtime.forward.target_session … state=4`。

### 8.4 出口落地（egress，`bridge.c` + `runtime.c:3920+`）

egress 解出 `XGB1` 载荷后，由 `xgw_bridge_egress_handle_payload`：
- `TCP_OPEN` → `tcp_connect_ipv4(host,port)` 建真实 TCP；`TCP_DATA` 转发字节。
- `UDP_OPEN` → `udp_connect_ipv4(host,port)`（connected UDP，IPv4 only，连接超时 500ms）；
  `UDP_DATA` 收发。回包经 `bridge_egress_drain_socket` 反向封 `XGB1` 送回。

---

## 9. 运行时主循环（`runtime.c`）

主循环每轮：
1. `runtime_channel_manager_tick` — 对 active/candidate/draining 三类线路、PREVIOUS/NEXT 两个方向、
   3 个 slot，做握手 / keepalive / 提升 established。
2. 轮询输入：UDP / TUN / bridge ingress / bridge egress / AF_XDP / 路由控制文件。
3. `stream_sched_pick` — 按优先级 + DRR（deficit round robin）+ 饥饿提升挑一个流发送；
   受 `send_credit_bytes`、`session_budget`（按 cwnd 加权份额）、`ack_credit` 门控。
4. `traffic_limiter_consume` — 令牌桶基线/突发限速（若配置）。
5. keepalive / 控制会话刷新 / FEC reap / route tick / GC。

调度器细节（`xgw_stream_sched_entry_t`）：每流维护 `parity_budget`、`feedback_cadence_ms`、
`rolling_loss_ppm`/`rolling_recovered_ppm`，按返回情况自适应升降 FEC 与 ACK 节奏。
流量类（`flow_class`）：`2=control 3=media 8=media_bootstrap`，分别给更高优先级 / 更密 ACK。

---

## 10. 策略与防护（`policy.c`）

- `xgw_allow_policy`：静态 CIDR、精确域名、后缀域名、带 TTL 的动态学习 IPv4、保守模式、grace period。
- `xgw_dos_config`：全局/单 IP 连接数上限、每秒速率 + 突发、黑名单时长、白名单 IP。
- 线上 `tmp/tmp-*.conf` 当前 `dos_enabled=false`、`allow_cidrs=0.0.0.0/0`（放通），
  靠前端 HY2 token 做准入。

---

## 11. 传输层注意点（`transport_udp.c`）

- `xgw_udp_send_ex` → `sendto`；`SO_RCVBUF`/`SO_SNDBUF` 可调。
- **未设置** `IP_DONTFRAG` / `IP_MTU_DISCOVER`，也未做 PMTUD（`pmtud_disabled` 仅记录在协商日志）。
  MTU 完全依赖 profile 静态配置（1380）。详见诊断文档对 MTU 的分析。
- Windows 下处理 `SIO_UDP_CONNRESET`。

---

## 12. 构建与自测

```bash
# Linux 构建（部署脚本在 ingress 机上执行同样命令）
make            # 或见 Makefile / deploy_fix.py:compile_on_ingress

# 本地自测（Windows 可跑）
./xgw selftest sample/live-bbr.conf   # 验证分片/FEC 恢复/replay
./xgw run sample/live-bbr.conf        # UDP 路径运行
./xgw outbound-smoke <cfg> tcp host:port
```

部署见 `docs/hy2bridge-deploy.md` 与 `tools/deploy_fix.py`：
打包源码 → ingress 编译 → 同一二进制分发三机 → **按 egress→relay→ingress 顺序重启**
（避免 ingress 旧安全状态导致 `secure_open_fail`）。

> 当前实际部署目录以 `tools/deploy_fix.py` 为准，即 `WORK_DIR = "/etc/xgw"`。
> 若文档或人工操作仍沿用旧的 `/etc/xgw-hy2bridge` 路径，会出现“本地改了、线上没变”的错位。

---

## 13. 关键常量速查

| 常量 | 值 | 含义 |
|------|----|------|
| XGW_MAX_SESSIONS | 256 | 会话上限 |
| XGW_HEADER_SIZE | 20 | 主帧头 |
| XGW_FRAGMENT_HEADER_SIZE | 10 | 分片头 |
| XGW_FEC_HEADER_SIZE | 12 | FEC 头 |
| XGW_MAX_FRAME_SIZE | 2048 | 内部帧缓冲 |
| XGW_MAX_FRAGMENTS | 64 | 单组最大数据分片 |
| XGW_REPLAY_BITMAP_WORDS | 4 | 256-bit replay 窗口 |
| 加封开销 | 24 | nonce(8)+tag(16) |
| live-bbr pacing | 50 Mbps | 默认 pacing |
| UDP connect 超时 | 500 ms | egress UDP 落地 |

---

## 14. 已知缺陷与风险（与诊断文档联动）

1. ~~**HY2 前端 UDP 不走 xgw 链路**~~ ✅ **已修复（2026-06-23）**：`bridgeOutbound.UDP` 现经
   bridge TCP 落地（`xgwBridgeUDPConn`），UDP 出口随 TCP 一同走 ingress→relay→egress(KZ)。
   见 `docs/diagnosis-shadowrocket-tiktok.md` §1。
2. ~~**MTU 无 PMTUD**~~ ⚠️ 部分缓解：profile 默认与部署配置已收紧到 1280/1100；
   真正的 PMTUD 仍未实现（中长期项）。
3. **`IgnoreClientBandwidth: true`**：前端仍忽略客户端带宽协商（官方 core 默认）；
   如需 Brutal 由客户端拉速，需评估后再关闭。
4. **部署目录与脚本入口需统一**：当前以 `tools/deploy_fix.py` 和 `/etc/xgw` 为准；
   若仍使用旧的 `hy2bridge` 文档路径或旧脚本名，运维很容易跑到错误目录。
5. **闲置首连超时** ✅ **已修复（2026-06-23）**：前端 QUIC `MaxIdleTimeout=120s`、
   `UDPIdleTimeout=300s`，后端 `max_idle_timeout_sec` 默认 120s。见诊断文档 §5b。
6. session 表 O(N) 线性查找；replay 窗口硬编码 256；均为规模上限而非当前瓶颈。
</content>
</invoke>
