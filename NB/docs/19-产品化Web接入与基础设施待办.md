# 产品化 Web 接入与基础设施待办

## 1. 已确认决策

1. C 数据面保持独立；现有 Web 作为用户、套餐、线路、凭据元数据、用量和监控的业务系统。
2. NB 增加 Go 管理适配层，负责 SQLite 缓冲、幂等上报、Web API、指标聚合和运维编排；不在 C 转发热路径中访问 Web 或数据库。
3. 当前只有一台 Entry，先支持同机多 worker；多 Entry 拓扑和部署模型可以开发，但在线验收等待第二台 Entry。
4. 多线路批量工具可以继续完善，真实批量准入等待至少一条新线路。
5. IPv6 延期；大 UDP 提高优先级，先交付最小 Xray/NB transport shim。

## 2. 现有 Web 对接边界

Web 可以直接承接持久化、计费与监控。NB 侧使用统一 HTTPS JSON 接口，不依赖 Web 的实现语言。

### 2.1 NB 上报到 Web

| 接口 | 用途 | 幂等键 |
|---|---|---|
| `POST /api/nb/v1/usage-events` | 节点/worker/租户流量增量 | `node_id:worker_id:boot_id:sequence` |
| `POST /api/nb/v1/node-snapshots` | health、deployment、profile、容量和 FEC 状态 | `node_id:observed_at` |
| `POST /api/nb/v1/line-snapshots` | 跨节点线路汇总 | `line_id:window_end` |
| `POST /api/nb/v1/incidents` | 告警和 incident bundle 元数据 | `incident_id` |
| `POST /api/nb/v1/provision-results` | 开线、升级、回滚和证书结果 | `operation_id` |

上报必须使用 TLS、服务身份认证、请求签名或短期 bearer token；失败时写入本机 SQLite outbox，指数退避重传。Web 返回成功前不得删除 outbox 记录。用量采用增量事件并由 Web 幂等入账，不能用可回退的瞬时累计值直接出账。

### 2.2 Web 下发到 NB

推荐由 NB 管理适配层提供受限 API，Web 调用：

- `POST /v1/users`：开通用户；
- `PATCH /v1/users/{id}`：停用或恢复；
- `PUT /v1/users/{id}/plan`：修改套餐和账期；
- `PUT /v1/users/{id}/route`：选择固定出口线路；
- `POST /v1/lines/{id}/deployments`：部署已签名版本；
- `POST /v1/lines/{id}/rollback`：回滚到明确的 deployment；
- `GET /v1/operations/{id}`：查询异步操作结果。

所有写操作必须带 `Idempotency-Key`，并记录操作者、请求摘要、前后状态和结果。Web 不得直接 SSH 执行任意命令。

### 2.3 Web 监控展示

Web 可以直接展示跨线路看板和告警列表。NB 上报至少包含：

- `line_id`、`node_id`、`role`、`worker_id`、`deployment`、`profile`；
- health、在线 worker、会话数、吞吐、容量利用率；
- queue age、event-loop delay、effective loss、reorder、异常关闭；
- FEC `observe/active`、repair/retx 计数；
- collection lag、outbox backlog、数据库错误和证书到期时间。

告警状态由 Web 保存和展示；独立通知通道仍建议交给 Alertmanager 或 Web 已有的告警服务，避免页面不可用时同时失去通知。

## 3. 用户、证书和密码

当前开线流程可以继续生成测试节点证书和随机用户密码，但必须区分两类身份：

1. 节点证书用于 Entry/Middle/Exit 的 mTLS，不分发给普通代理用户；
2. 用户凭据用于 SOCKS/NB 客户端认证，可由开通流程生成并一次性显示给用户。

Web 普通业务库可以保存 `credential_id`、用户名、密码算法/版本、证书指纹、有效期、状态和最后轮换时间。明文密码和私钥不得进入普通字段、日志、incident bundle 或监控标签。过渡期如必须由 Web 再次展示密码，应使用独立 KMS/Vault 包络加密字段并记录每次解密审计；产品默认应只保存强密码哈希，丢失后重置而不是找回。节点私钥由 Vault/KMS 托管，Web 只保存引用。

## 4. 最小 Xray/NB 大 UDP shim

`client/xraydemo/nbproto/udp_frag.go` 提供与 C 服务端 `NBUD v1` 一致的分片和乱序重组：

- 单个逻辑 UDP datagram 最大 65,507 字节；
- 每片 payload 最大 1,000 字节，最多 66 片；
- 支持乱序、重复片去重、超时回收和最多 8 个并行重组槽；
- 分片必须承载在完成 NB 认证的 QUIC 数据面中，不能把 demo TCP framing 暴露到公网。

后续接入 Xray outbound 时，outbound 从 Xray 接收完整 UDP datagram，调用 `FragmentUDP` 后立即逐片写入认证 QUIC 会话；接收端用 `UDPReassembler` 恢复完整 datagram 再交回 Xray。不得等待多个 datagram 凑批后发送。

## 5. 等待资源后统一唤起的任务

以下任务保留为明确的基础设施待办，不以当前 KZ 单线路结果冒充完成。

### INFRA-ENTRY-02：第二台 Entry

需要提供：主机名、地区/运营商、SSH 地址端口、bootstrap 凭据、内外网地址、CPU/内存、带宽、允许端口和预期接入域名。

资源到位后执行：主机身份固定、原子部署、`entries[]` 双节点拓扑、健康摘除、DNS/客户端多端点、新连接切换、节点故障矩阵、固定 KZ 出口确认。已有 SOCKS 会话跨 Entry 宕机续传不在本项内，依赖正式客户端协议。

### INFRA-LINE-02：新增线路

需要提供：Entry/Middle/Exit 清单、线路 ID、地区与运营商、带宽套餐、预期固定出口 IP、MTU/IPv4 能力、端口策略、维护窗口和 secret 引用。

资源到位后执行：静态资格检查、真实三跳负载探针、稳定 profile、原子部署/回滚、TCP/UDP 完整性、三角色故障矩阵、容量与计费核对、跨线路汇总。第二条线路通过后再扩大到至少三条线路做批量验证。

### INFRA-WEB-01：现有 Web 接口

需要提供：测试/生产 base URL、TLS 域名、鉴权方式、接口负责人、用户/套餐/线路现有字段、账期规则、时区、告警接收通道和测试租户。

资源到位后执行：字段映射、契约测试、outbox 重放、幂等入账、开通/停用/改套餐/选线路闭环、看板与告警验收。不得在资料未齐时硬编码 Web 地址或 token。

### INFRA-SECRET-01：正式密钥存储

需要确定 Vault/KMS 地址、认证方式、命名空间、备份恢复责任人和 break-glass 流程。资源到位后迁移节点私钥、SSH CA 和可找回用户凭据；迁移前继续使用 gitignored 私有目录，但禁止提交或输出秘密值。

## 6. 唤起规则

任一资源到位时，引用对应任务 ID 并提供该节字段即可启动；一次提供多项资源时，按 `SECRET/WEB -> ENTRY/LINE -> 部署验证 -> 计费监控验收` 的顺序执行。每条新线路使用独立报告目录、deployment、profile 和回滚点，不覆盖 KZ 历史证据。
