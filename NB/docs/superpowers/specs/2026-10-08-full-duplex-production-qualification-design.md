# 生产线路 10 Mbps 全双工独立容量与资格门禁设计

## 背景

生产线路必须提供上下行各 10 Mbps，两个方向独立保障、独立核算。单方向分别能够达到 10 Mbps，
不能证明线路具备 10 Mbps 全双工能力。

`gz-hk-sp-00001` 的现场证据表明现有模型会产生误判：

- 单独上行 60 秒达到 9.97 Mbps；
- 单独下行 60 秒达到 10.00 Mbps；
- 20 秒双向并发依靠链路突发额度达到接近 10+10 Mbps；
- 90 秒双向并发只有上行 8.36 Mbps、下行 0.74 Mbps；
- 广州到香港段 RTT 从约 4.4 ms 上升到 p95 24.17 ms，20 个窗口中 13 个 blocked；
- 香港到西班牙段 blocked 为 0，p95 有效丢包为 0.017%；
- 广州和香港主机没有本地 `tc` rate class、网卡 drop 或 error。

该链路表现为约 10 Mbps 双向合计的云网络 policer，并带约 45 秒突发桶，不满足生产要求。

## 目标

- 生产线路必须在同一个 90 秒窗口中同时验证上行和下行。
- 每个方向的有效业务吞吐必须达到配置速率的 95%。10 Mbps 线路的通过线为 9.5 Mbps。
- 每一跳的上行、下行容量独立建模、预留和核算，不能用双向合计容量替代。
- 未通过资格验证的线路不能进入 `active`、不能交付客户端配置、不能进入生产计费。
- 协议调优只能在资格验证通过后提交，不得用调优掩盖物理容量不足。
- 生产线路只能引用生产设备和已声明全双工容量的有向链路。
- 保留历史 API、任务和线路数据的读取兼容，不修改 NB wire format。

## 非目标

- 不在控制面内模拟或替代云厂商扩容。
- 不自动把双向合计 10 Mbps 推导成每方向 10 Mbps。
- 不把 `resource_group` 恢复为容量或锁的权威来源。
- 不因资格失败自动降低客户套餐或修改计费速率。
- 不在本次工作中修改 Node wire version。

## 容量模型

新增物理有向链路资源，身份为：

```text
from_device_id + from_role + to_device_id + to_role
```

每条物理连接保存两个独立容量：

```json
{
  "forward_capacity_mbps": 10,
  "reverse_capacity_mbps": 10,
  "billing_mode": "independent-directions",
  "environment": "production",
  "status": "ready"
}
```

`forward` 表示 `from -> to`，`reverse` 表示 `to -> from`。`billing_mode` 只接受：

- `independent-directions`：两个方向独立保障，可用于生产；
- `aggregate-bidirectional`：双向合计容量，不可承载要求全双工的生产线路；
- `unknown`：未确认，不可用于生产。

线路的每一段都按实际拓扑形成容量需求：

- Entry -> Relay：预留线路 `upstream_mbps`；
- Relay -> Entry：预留线路 `downstream_mbps`；
- Relay -> Exit：预留线路 `upstream_mbps`；
- Exit -> Relay：预留线路 `downstream_mbps`。

可用容量按方向分别计算：

```text
forward_available = forward_capacity - sum(active_or_reserved.forward_demand)
reverse_available = reverse_capacity - sum(active_or_reserved.reverse_demand)
```

同一线路重试必须复用原预留，不得重复扣减。删除、资格失败后的显式释放和强制删除都必须幂等。

## 数据模型

新增 `network_links`：

- `id`
- `from_device_id`
- `from_role`
- `to_device_id`
- `to_role`
- `forward_capacity_mbps`
- `reverse_capacity_mbps`
- `billing_mode`
- `environment`
- `status`
- `created_at`
- `updated_at`

新增 `line_capacity_reservations`：

- `line_id`
- `link_id`
- `forward_mbps`
- `reverse_mbps`
- `state`: `reserved`、`active`、`released`
- `operation_id`
- `created_at`
- `updated_at`

新增 `line_qualifications`：

- `line_id`
- `operation_id`
- `deployment_id`
- `status`: `pending`、`admitted`、`rejected`
- `duration_seconds`
- `required_ratio`
- `target_upstream_mbps`
- `target_downstream_mbps`
- `achieved_upstream_mbps`
- `achieved_downstream_mbps`
- `reasons`
- `evidence`
- `created_at`
- `updated_at`

历史 `line_specs.upstream_mbps` 和 `downstream_mbps` 继续作为套餐方向速率的权威值。

## 生产开线状态机

生产线路采用两阶段开线：

```text
draft
  -> provisioning
  -> qualification_pending
  -> active
```

失败路径：

```text
provisioning -> maintenance
qualification_pending -> qualification_failed
```

流程：

1. 校验线路和全部设备的 `environment=production`。
2. 校验两段物理链路均存在、状态为 `ready`、计费模式为 `independent-directions`。
3. 在数据库事务中分别检查两个方向的剩余容量并创建 `reserved` 预留。
4. 执行实例级开线，不修改共享 Node 运行时。
5. 开线成功后状态进入 `qualification_pending`，不交付客户端配置。
6. 自动创建一个与 deployment 绑定的 `line.optimize` 任务。
7. 运行 90 秒并发上行、下行探针。
8. 两方向分别达到 95%，且完整性、节点健康和链路证据均通过，才提交 transport profile。
9. 三节点 readback 成功后，将预留状态改为 `active`，线路状态改为 `active`，开放客户端配置。
10. 任一方向未通过时，记录 `qualification_failed`，保留实例用于诊断，但不激活预留、不交付客户端配置。

测试线路继续允许人工开线和验证，但必须明确显示为测试环境，不能占用生产链路容量或生成生产可用配置。

## 全双工资格判定

固定参数：

```text
duration_seconds = 90
required_ratio = 0.95
probe_mode = concurrent-full-duplex
```

每方向判定：

```text
upstream_pass = achieved_upstream_mbps >= target_upstream_mbps * 0.95
downstream_pass = achieved_downstream_mbps >= target_downstream_mbps * 0.95
```

资格通过必须同时满足：

- payload integrity 为 `ok`；
- 上行 `integrity=count-ok`；
- 下行 `integrity=count-ok`；
- 下行窗口完整或在固定窗口内有可解释的正常结束；
- 上行达到 95%；
- 下行达到 95%；
- 三个角色健康；
- 没有实例级队列溢出、运行时 reset 或端口冲突。

单向测试只作为诊断证据，不参与生产 admission。

若资格拒绝，任务结果必须明确列出：

- `insufficient-uplink`
- `insufficient-downlink`
- `payload-integrity`
- `runtime-queue-overflow`
- `device-unhealthy`

资格拒绝属于业务判定，不是 worker 异常，不得进入 profile rollout。

## 客户端配置与计费门禁

- `line.open` 可以生成加密客户端凭据，但在资格通过前不得通过 API 或 Web 展示连接 URL、二维码或密码。
- `attachClientConfig` 必须校验线路状态为 `active` 且最新 qualification 为 `admitted`。
- 资格失败后重试必须复用同一客户端凭据，避免泄漏或重复生成账户。
- 计费系统只能读取 `active` reservation；`reserved` 和 `qualification_failed` 不产生生产计费。
- 计费记录分别保存上行承诺 Mbps 和下行承诺 Mbps，不能折叠为一个 `capacity_mbps`。

## Web 操作

新增网络链路管理页面或设备拓扑中的链路编辑入口，使用明确的两个数值输入：

- 正向保障 Mbps
- 反向保障 Mbps

同时显示：

- 计费模式；
- 环境；
- 已预留/可用的正向容量；
- 已预留/可用的反向容量；
- 占用该链路的线路列表。

线路列表增加资格状态：

- 待验证；
- 全双工合格；
- 全双工不合格；
- 资源不足。

失败详情同时显示目标、实测、95% 门槛和失败方向，不显示模糊的“测速失败”。

## 历史线路治理

上线后扫描所有生产线路：

1. 生产线路引用测试设备时标记 `maintenance_required`。
2. 缺少物理链路资源记录时标记 `capacity_unknown`。
3. 链路为 `aggregate-bidirectional` 时标记 `full_duplex_unqualified`。
4. 没有 95% 并发证据或证据早于当前 deployment/profile 时标记 `qualification_required`。
5. 不自动停用已有流量，但禁止新增客户端交付、升级和扩容，直到治理完成。

`gz-hk-sp-00001` 首次迁移结果应为：

- 线路环境：生产；
- Entry `gz-55`、Relay `HK-151`：当前设备环境为测试，需改为生产或更换设备；
- 广州↔香港链路：当前实测为双向合计约 10 Mbps，不得登记为每方向 10 Mbps；
- 资格状态：`full_duplex_unqualified`；
- 当前 generation 5 保留，不自动继续调优。

## 并发与隔离

- 容量预留使用数据库事务和唯一键，避免并发开线超卖。
- 共享实际 `device_id + role` 的验证任务继续串行。
- 同一物理链路上的生产资格探针串行，避免互相污染。
- 不同物理链路允许在 worker 并发上限内并行。
- Node 构建锁和共享运行时升级锁保持现有语义。

## 错误处理

- 容量不足在开线前返回 409，并列出缺少容量的方向和数值。
- 链路未登记、计费模式未知、设备环境不匹配均在写远端配置前失败。
- 开线实例成功但资格失败时保留远端实例和预留，供重试；管理员可选择清理并释放。
- 删除操作先清理远端实例，再原子释放全部方向预留并删除线路记录。
- 强制删除记录可能残留的远端实例和已释放容量审计，不得留下占用中的 reservation。

## 测试

### 中央存储

- 正向和反向容量分别扣减。
- 两条并发线路不能超卖同一方向。
- 相反方向有容量不代表当前方向有容量。
- 重试不重复预留。
- 删除和失败清理幂等释放。
- SQLite 和 MySQL 语义一致。

### Worker 与探针

- 90 秒上下行仍并发执行。
- 9.49/10 Mbps 拒绝，9.50/10 Mbps 通过。
- 一个方向通过、另一个方向失败时不得生成 profile。
- 固定窗口 evidence 保留目标、实测、完整性和结束原因。
- 同链路验证串行，不同链路验证并行。

### Web/API

- 生产线路不能选择测试设备。
- 未登记链路和 aggregate 链路不能创建生产开线任务。
- 资格通过前客户端 URL/二维码不可读取。
- 资格失败详情显示准确方向和门槛。
- 历史任务和测试线路继续可读。

### 生产验收

选择一条物理上已提供 10 Mbps 全双工的线路：

1. 连续三次 90 秒并发验证；
2. 每次两个方向均不少于 9.5 Mbps；
3. 无 q2t/QTX 溢出、reset 或主机丢包；
4. 通过后客户端配置才可见；
5. reservation 两个方向分别为 10 Mbps；
6. 删除后两个方向容量完整释放。

## 发布与回滚

- 先发布数据库与只读 API，再发布容量配置 UI。
- 完成链路资源录入后启用生产开线硬门禁。
- 最后启用历史线路治理限制，避免一次性阻断全部旧线路。
- 回滚控制面时保留新表和审计数据，不删除 reservation。
- 本设计不修改 Node 二进制；若实施过程中必须修改 `src/`，Node 版本必须升级为
  `V200R001C01 / 2.1.1`，并通过独立共享运行时升级流程部署。
