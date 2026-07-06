# Newbility (NB) 设计文档

## 1. 背景与目标

xgw 自研三跳 UDP 隧道跑 TikTok 出现"无网络连接",而**相同三台物理机、相同 RTT** 的纯 hy2 正常。
根因是**乘性放大**: 自研协议每跳的单线程调度 / pacing 自旋 / recv 轮询 / FEC / 加密开销,被 TLS 握手往返 × 跳数放大 6–12×,透明隧道不终结 TLS 无法消除。

结论: 不再打补丁,直接**内嵌成熟 QUIC(picoquic)** 作为传输内核,保留自研的 BBRv2 拥塞控制与(规划中的)FEC。
新协议命名 **Newbility(NB)**,角色(entry/middle/exit)与角色名保留,内核替换。

目标: 优于纯 hy2、协议自研可控、适配 TikTok 直播与拍卖实时 TCP 流。

## 2. 架构

一份二进制 `nb_node`,`-r` 选角色。自定义事件循环驱动 picoquic:
```
select(udp_fd + tcp_fds, timeout=picoquic_get_next_wake_delay)
  -> recvfrom + picoquic_incoming_packet          (UDP -> QUIC)
  -> accept / pump_tcp / flush_q2t                 (TCP IO)
  -> picoquic_prepare_next_packet + sendto         (QUIC -> UDP)
```

统一数据模型 `proxy_stream`(left=靠 client,right=靠 target):

| 角色 | left | right | 回调命中 |
|------|------|-------|----------|
| entry  | TCP(客户端)   | 下游 QUIC(第一跳) | down(回程) → 写 TCP |
| middle | 上游 QUIC(上一跳) | 下游 QUIC(下一跳) | up(去程)→转 down;down(回程)→转 up |
| exit   | 上游 QUIC(上一跳) | TCP(目标)     | up(去程) → connect + 写 TCP |

回调按 `(cnx, stream_id)` 双向查找配对(`ps_find_by_up` / `ps_find_by_down`),
去程 `fwd_down` / 回程 `fwd_up` 统一转发;双向 fin + 缓冲排空后 `ps_free` 归还 stream 额度。

## 3. 协议: source-route(动态选路地基)

stream 首部第一行 = 接收节点出发的剩余路径:
```
H:hop:qport,...,T:target:port
```
- middle 消费第一段 `H:host:port` → **动态**连该地址(不预配下一跳)、转发剩余 route;
- exit 消费 `T:host:port` → connect 目标。

**中间节点无状态**: 路径完全由 entry 下发,契合产品"卖线路(起终点明确)+ 中间路径动态优化/探测选最优/跳数可变"。
控制平面(规划)只需给 entry 下发 route 串即可切换链路。

## 4. 传输参数与拥塞控制

- 拥塞: `picoquic_bbr_algorithm`(BBRv2 + BBR3 bugfix),loss-aware。
- 传输参数(放开一条 QUIC 承载大量并发 TCP 流,修默认 `max_streams_bidi≈3` 致 stream 阻塞):
  - `initial_max_streams_bidi = 2000`
  - `initial_max_stream_data_bidi_local/remote = 1 MiB`
  - `initial_max_data = 16 MiB`

### BBRv2 + FEC 协调(规划)
FEC 纠错上限 K/N 天然是分界线: 随机丢包(≤K,FEC 补,BBRv2 不感知退避) vs 拥塞丢包(突发>K,透传给 BBRv2 退避)。
自适应 FEC 平时 K=0,探测到随机丢包再开。自研 FEC 走独立 misc_frame + 复用 RS 编码。

## 5. 可观测性(log4c)

全量 `log4c_*`,每 stream 带 `id + up_sid/down_sid + route`,三跳可 `grep` 追踪同一流:
```
entry  id=1 accept -> down_sid=0  route=H:kz:4443,T:target:80
middle id=1 up_sid=0 -> nexthop kz:4443 down_sid=0 rest=[T:target:80]
exit   id=1 sid=0   -> target target:80
```
未来控制平面据此做路径质量度量与选路。

## 6. 第三方依赖: picoquic vendor(多平台)

**决策**: picoquic 不依赖机器预装目录,作为受控第三方**随 NB 项目走**;因需多平台,**源码合并进项目 + 按目标平台构建 + 预编译静态库作缓存**。
```
third_party/picoquic/
  src/                   picoquic 完整源码(picoquic + picotls + cifra/micro-ecc/picotest 子依赖, ~8.8MB)
  prebuilt/<platform>/   各平台预编译静态库缓存(如 linux-x86_64/*.a)
```
构建系统(规划 CMake): 目标平台有 prebuilt 直接链,否则从 src 现编并落缓存。
`tools/vendor_picoquic.py` 从构建机抽取固化。**现阶段** x86_64 已有 prebuilt,未做完整源码 vendor(先验证功能)。

## 7. 验证结果(2026-07-04)

### 单机多 stream(kz)
10 并发 stream 全 http=200(修复前 try3+ http=000): 根因 = `max_streams_bidi` 默认过小 + stream 未释放;
根治 = 放开传输参数 + `got_remote_fin && tcp_eof && q2t 排空` 后 `ps_free` 回收。

### 真实地理三跳 gz→hk→kz→target
- 5 并发 stream 全 http=200;300KB md5 `9696…8277` 零损坏穿三跳;
- first_byte: 冷启动 ~400ms(含握手),复用 ~200ms;
- middle 正确 `H:` 逐跳消费转发(日志坐实在链路中,非绕过)。

## 8. 后续

- picoquic 源码 vendor + CMake 多平台构建(第 6 节落地);
- first_byte 优化(0-RTT/TLS session 复用)对标 hy2;
- 自研 FEC(第 4 节);
- 控制平面: 探测 + 加权最短路选路 + source-route 下发(产品"卖线路")。
