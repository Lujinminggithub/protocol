# NB 路线图 / 待办

> [01-需求](01-requirements.md) | [02-架构](02-architecture.md) | [03-已实现](03-implementation.md) | [04-部署运维](04-deployment-ops.md) | 05-路线图(本篇) | [06-自研客户端](06-custom-client.md) | [fec-plan](fec-plan.md)

## 已完成（里程碑）
雪崩/泄漏/截断根治、P0-1异步DNS、P0-2多连接池、P0-3流优先级+直播专用道、窗口8MiB/buffer32MB、P1-2批量收发(recvmmsg+GSO)、快速ACK、日志降级、白名单(IP/域名/端口+热配)、链路质量观测(linkq)。手机验证：TikTok视频/直播/baidu正常。

## 待办优先级

### P0 — FEC 收尾与量化（抗抖动）
- 详见 [fec-plan.md](fec-plan.md)。
- 根因：三跳任一跳丢包→QUIC重传恢复≈1.5×RTT×跳数→直播卡/黄红。**已用 linkq 定位：hk→kz 跨境段丢包高15-20倍、RTT~200ms(gz→hk仅~50ms)是瓶颈**。
- 当前已落地版本：仅直播延迟流(prio=4)在 `middle→exit` 去程双发、`exit→middle` 回程双写，并在 exit/middle 按 `flowid` 做字节级去重；默认关闭，middle 配 `NB_FEC=on` 启用。
- 下一步不再是“从零设计”，而是：
  1. 用 `tc netem` 人工注丢包，量化直播毛刺/卡顿改善幅度；
  2. 评估双发带宽翻倍的副作用，决定是否把 `LAT_LANES 2→3` 或放宽 BBR loss 阈值；
  3. 从“middle 硬编码开关”演进到“按本跳链路质量自适应”。

### P1 — 产品化必需
1. **用户认证鉴权**（卖线路前提,当前无认证靠端口挡）。自研协议/客户端加token握手。见06。
2. **限速能力(QoS)**：见下§限速。依赖认证。
3. **自研客户端**（替代Shadowrocket）：见[06](06-custom-client.md)。增值:认证/业务元数据/客户端分流前移/省一跳。
4. **控制平面**：探测各中转质量+加权最短路选路+source-route下发。linkq是质量度量基础。**已发现hk→kz线路差,选路可换更优exit落地点**。

### 限速能力（QoS）— 卖线路必需，分两层互补
**① 用户套餐限速(按用户,核心)→ NB应用层自研**
- 每用户X Mbps。entry认证后知道用户身份→对该用户所有stream聚合**token bucket**。
- 实现:每用户一个桶(rate+burst),事件循环补token。执行点在数据泵(pump_tcp/fwd_down/fwd_up)——超速就暂缓读TCP/暂缓add_to_stream,靠QUIC flow control自然背压(NB已有q2t背压)。是"减速"非"丢弃"。
- 与优先级协同:限速额度内直播(prio=4)优先,批量让路。
- **依赖认证**:用户身份来自token握手。当前SOCKS5无身份,只能按IP限→认证是前置。

**② 节点/IP级限速防护(粗粒度)→ 复用xgw XDP**
- 节点总带宽上限、异常IP限速、防UDP洪水。
- XDP(eBPF网卡层)与协议无关,xgw/bpf/的per-IP token bucket可**直接部署到NB机器**(限UDP/IP,不关心里面是NB的QUIC)。高性能省CPU。
- **局限**:QUIC加密,XDP看不到内部用户/业务,**替代不了**用户套餐限速。

**关键判断**:用户套餐限速必须NB应用层(用认证身份+优先级);节点/IP防护复用xgw XDP且不依赖认证可先用。顺序:认证→用户套餐限速(应用层)→XDP节点防护(复用xgw)。

### P2 — 优化/规模化
1. **优化4**:exit DNS多地址重试;直播webcast媒体vs rtc信令细分优先级。
2. **多线程SO_REUSEPORT**(多用户规模化):单用户单核够80Mbps。**可做成多线程**——每线程独立quic context(picoquic非线程安全)+SO_REUSEPORT内核分发,共享配置/白名单/DNS缓存,比多进程优。DNS worker已是线程先例。
3. **P1-1 epoll+hashmap**:替select(FD_SETSIZE=1024上限)+O(n)查找。高并发(数千fd)才需要,单用户价值低,风险最高,放最后。
4. **0-RTT/session复用**:降首字节延迟,对标hy2。
5. **q2t内存收缩**:只增不减,大流后不释放。
6. **picoquic源码vendor+CMake多平台**:现只x86_64 prebuilt。

## 演进建议顺序
1. **FEC 量化/自适应**(直播抗抖动收尾) 2. **认证+限速+自研客户端**(产品化) 3. **控制平面选路**(卖线路核心) 4. 规模化(多线程)/epoll(用户量上来再做)

## 重要判断记录
- 直播红黄根因:①白名单IP误拦(已修) ②**hk→kz跨境段丢包严重**(linkq实测,需FEC/换线路)。
- 单用户单核跑80Mbps够,规模化才多核。
- SOCKS5是哑管道(传不了元数据/认证),自研客户端突破这个是产品化关键。
- 限速:用户级必须应用层(QUIC加密XDP看不到),节点级可复用xgw XDP。
