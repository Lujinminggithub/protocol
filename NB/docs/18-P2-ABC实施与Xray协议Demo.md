# NB P2-A/B/C 实施与 Xray 协议 Demo

## 1. 阶段基线

- P2 从 Git tag `v1.5`（提交 `48d36a8`）启动。
- P2 当前只包含服务端与发布体系的 A/B/C，不启动手机 App 开发。
- 手机端未来基于 Xray-core；当前只交付可复用的协议编解码包、黄金向量和收发 demo。

## 2. P2-A：UDP/PMTU 完整性

状态：`closed_restricted_ipv4`。已加入每连接 PMTU 确认状态、连续提升确认、实际 MTU 降低后的黑洞回退和 60 秒冷却；KZ IPv4、黑洞回退与 UDP 分片已有真实证据。三端均无 IPv6 默认路由，本次发布通过签名风险豁免限定为 IPv4-only，IPv6 不标记为测试通过。

目标是把开线前静态 MTU 推荐升级为运行期闭环：

1. 运行期 PMTU 探测和每跳结果缓存；
2. UDP 分片边界、乱序、重复、超时和黑洞回退；
3. IPv4/IPv6 与 SOCKS5 UDP ASSOCIATE 完整语义矩阵；
4. PMTU 变化不破坏既有会话，异常必须有指标和关闭原因。

## 3. P2-B：FEC 生产化

状态：`protocol_matrix_passed_observe_only`。T1 无丢包、T2 单缺片恢复和 T3 NACK/RETX 已在 KZ 同一 deployment 通过；`tools/fec_canary.py` 已实现三轮配对测量、完整性、延迟收益、带宽、CPU、内存和恢复量的默认拒绝判定。当前 K=4/R=2 的理论及观测 repair 开销约 50%，高于 35% 准入上限，因此仍禁止生产启用。

FEC 继续保持 observe-only，满足以下条件后才允许 canary：

1. 多 block 乱序、BDP history、NACK/RETX/ACK、FIN/RST 完整回归；
2. source/repair 丢包、突发丢包、重排序和 RTT 阶跃 netem 矩阵；
3. 量化恢复率、额外带宽、CPU、内存和端到端延迟收益；
4. 明确启用、回滞、降级和自动关闭阈值，收益不足时保持关闭。

## 4. P2-C：CI、混沌与 soak

状态：`closed_restricted`。Release、sanitizer、仓库 gate、三跳 loopback 和 Xray 协议 demo 已进入 CI；`tools/p2_release_gate.py` 同时支持完整证据的通用发布和 HMAC 签名、证据哈希绑定、有限期的受限发布。三角色故障恢复不可豁免；IPv6、active FEC 和未完成的 24 小时 soak 只能在固定限制下显式接受。

目标是把目前的本地回归提升为可重复的发布门禁：

1. Release、ASan/UBSan、P0/P1/P2 gate 和三跳 loopback；
2. netem、worker 重启、断网、路由熔断、证书失败和精确回滚；
3. 受控 canary 与长时间 soak 报告；
4. 报告携带 commit、deployment、profile 和场景参数，失败不可被 trap 覆盖。

## 5. Xray 客户端边界

Xray-core 负责未来手机端的 TUN、DNS、规则路由和连接生命周期；NB 适配层负责 NB 认证、流元数据、QUIC 数据面、FEC 与 multipath。仅靠 Xray JSON 配置不能获得自定义 NB 原生协议，后续需要把 NB outbound/transport 注册进所采用的 Xray-core 构建。

当前不实现 Xray fork、iOS/Android 外壳或生产 NB outbound，只提供：

- C 权威线格式 `src/nb_v2_metadata.c/.h`；
- Go 1.26 包 `client/xraydemo/nbproto`，供未来 Xray outbound 复用；
- `nbproto-demo` 的 encode/decode/send/serve 命令；
- C/Go 共用的固定黄金向量。

元数据中的 `session_id` 和 `flow_id` 不是认证凭证。生产 entry 只能在身份认证和版本协商完成后接受元数据，授权必须来自服务端绑定的认证上下文。

Demo 使用四字节网络序长度前缀承载单个元数据帧，目的只是跨语言联调；生产数据仍必须走经过认证和加密的 NB QUIC 会话，禁止把 demo TCP framing 或公网明文 SOCKS 当作生产协议。

## 6. P2 启动门禁

- P2-A/B/C 范围在路线图、差距表和本文件中一致；
- C 与 Go 对同一黄金向量编解码一致；
- 非法版本、标志、保留位、文本、截断和尾随字节必须拒绝；
- Release、ASan/UBSan 和 V1.5 三跳兼容回归保持绿色；
- 所有第一方代码文件继续不超过 1000 行。

## 7. KZ 首次部署与 soak

2026-07-24 已把 P2 首批实现原子部署到 `gz-83 -> hk-216 -> kz-1`，deployment 为
`aa35ad35ca3a9647-6849d6e3458a`。entry=1、middle=2、exit=2 worker 健康，固定出口验证为
`2.135.147.106`。FEC 运行时回读为 `observe=1, active=0`，没有为本次 soak 提前启用生产 FEC。

60 秒发布后 smoke 共 3/3 个 20 秒块通过，吞吐约 3.96Mbps，payload 完整，零采集错误、
零不健康 worker、零 deployment/profile 漂移。24 小时 soak 报告为
`build/soak/kz-p2-24h-20260724.json`，进程元数据为
`build/soak/kz-p2-24h-20260724.process.json`。首个 300 秒块通过：3.9973Mbps，队龄 P95 为 0、
峰值 895us，有效丢包 P95 为 0.051%，零采集错误和零不健康 worker。报告完成前状态保持 `running`，
不得提前作为 P2-C 的 24 小时通过证据。

## 8. KZ soak 故障关闭与恢复策略

首轮 24 小时 soak 在 chunk 154 失败并停止，失败前 153 个 300 秒块通过，累计有效负载
`45930.949s`。故障发生在 middle 到 exit 的 QUIC 连接短时停止推进后：旧恢复逻辑在队列持续
阻塞约 12 秒时执行 `pool-degraded-retire`，主动 teardown 活动流，Windows 负载端收到
`ConnectionResetError 10054`。故障报告和 incident bundle 保留为失败证据，不改写、不续跑。

恢复逻辑已改为 `quarantine + make-before-break`：

1. 阻塞连接进入 quarantine 后立即停止承接新流，并并行建立替代连接；
2. 替代连接 ready 后成为新流的 active 连接；
3. 旧连接进入 draining，既有流不再被池恢复逻辑主动 teardown；
4. 旧流恢复后继续传输，结束后才关闭旧连接；候选期间原连接恢复则取消候选；
5. 指标增加 `pool_recovery.quarantined` 和 `pool_recovery.mbb_promotions`。

deployment `49a60796c02ddefd-b5b91a6adab6` 已通过正式 CTest、三跳 payload、16 路并发、middle
worker 恢复和原子部署门禁，固定出口仍为 `2.135.147.106`，FEC 保持
`observe=1, active=0`。部署后的目标报告 `build/soak/kz-p2-mbb-targeted-20260725.json` 为
`passed`：单块 150MB、300.297 秒、3.996Mbps，前后 payload 和计数完整性通过，零采集错误、
零不健康 worker、零 deployment/profile 漂移。

经项目决策，不再从零执行另一轮 24 小时 soak，也不重复已经通过的长时块。原
`P2-C-24h-soak` 自动发布条件仍保持 fail-closed；本次决定记录为显式残余风险接受，不能把失败报告
或 300 秒目标报告伪装成完整 24 小时通过证据。逻辑流续传作为后续工作处理。

## 9. KZ P2-A/B/C 当前证据（2026-07-25）

当前原子部署为 `5057daee4c810d6f-6849d6e3458a`，profile 为
`gz-83-hk-216-kz-1:3`，固定出口仍为 `2.135.147.106`。entry=1、middle=2、exit=2
worker 健康，最终回读保持 `FEC observe=1, active=0`。

P2-A：60 秒 4Mbps 基线通过，实测两段 IPv4 DF MTU 为 1452，QUIC UDP payload MTU 为 1424；
受控黑洞报告 `build/p2/kz-p2a-blackhole-20260725-v2.json` 通过，同一逻辑流 MTU 从 1424
回退到 1252，恢复耗时 19.156 秒，45 秒 payload 完整。SOCKS5 UDP 真实测试覆盖 1、999、1000、
1001 字节并通过，其中 1001 字节跨 NB 内部分片；公网客户端发送 60000 字节时在到达 entry 前即因
公网 IP 分片受限而丢失，不能作为 NB 内部重组失败。KZ 三端没有 IPv6 默认路由，因此 IPv6 场景未通过，
P2-A 正式四场景报告不得标记为 passed。

P2-B：首次 active T1 暴露两个连续问题：历史窗口在 ACK 前被同步突发填满，以及 FEC datagram
发送队列超过 256KiB。发送端已改为由 history capacity、datagram 高水位和 BACK ACK 共同驱动的
增量泵送；满块立即发送，未满块从首字节起最多等待 1ms 后强制封块，事件循环把该 deadline 合并到
`epoll_wait`，不会等待下一批数据或 ACK；接收端 NACK 只对当前 HOL block 计时，避免后续块提前耗尽重试预算。最终 deployment 上：

1. T1 两轮 1MiB 无丢包响应哈希完整，START/ACK/双端统计齐全；
2. T2 三轮 1MiB 单 source 缺片响应哈希完整，middle 记录实际 recovered block；
3. T3 四轮 16KiB 超出 repair 能力的有界场景哈希完整，middle NACK、exit RETX send、middle RETX receive 均有证据；
4. 每一阶段及异常路径均在 finally 中恢复 `active=0`，远端日志按字节偏移读取，不再截断历史日志。

P2-C：最终报告 `build/p2/kz-p2c-fault-matrix-20260726.json` 已在 deployment
`6cb874fbb33ab487-6849d6e3458a` 通过 entry/middle/exit 三角色故障恢复矩阵；稳定数据恢复分别为
7.766 秒、9.594 秒、11.562 秒，均低于 15 秒 SLA，20 秒负载约 3.94--3.96Mbps，前后 payload、
profile 和固定出口一致。完整 24 小时门禁未通过，只能由下述有限期签名豁免接受。

## 10. P2 受限发布结论（2026-07-27）

P2-A/B/C 服务端范围已关闭，发布门禁采用 `p2-abc-v2` 受限路径。归一化证据由
`tools/p2_release_disposition.py` 生成，`tools/p2_release_waiver.py` 使用 HMAC-SHA256 签名并绑定
P2-A、P2-B、P2-C 故障矩阵和 soak 四份输入的 SHA-256；篡改、过期或证据漂移均 fail-closed。

本版本固定限制如下：

1. 仅准入 IPv4；原生 IPv6 PMTU/UDP 留待具备线路后补测，不声称已通过。
2. FEC 固定 `observe=1, active=0`；定量 canary 已证明当前 active FEC 不满足 payload、时延和资源准入，协议稳定性不依赖 active FEC。
3. 公网 UDP 单报文上限为 1001 字节；更大报文在获得独立公网分片证据前不属于本版本支持面。
4. 旧 24 小时 soak 保留为失败证据；153 个连续通过块、`45930.949s` 有效时长和修复后的 300 秒定向验证通过有限期签名豁免接受，不伪装为完整 24 小时通过。
5. 当前 deployment 为 `6cb874fbb33ab487-6849d6e3458a`，P2-C 三角色故障矩阵全部通过；该项不可豁免。

逻辑流续传、IPv6 通用准入和 active FEC 再准入转入后续版本。手机 App 仍未启动；本阶段客户端交付止于可供 Xray-core 适配使用的协议收发 demo。
