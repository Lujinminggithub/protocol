# 运行时端口声明与无冲突自动分配设计

## 目标

控制面分配端口时同时使用数据库线路规格和节点实际 shard 配置，避免未登记或库存漂移的运行实例造成端口冲突。

## 数据模型

新增 `runtime_port_claims`：`worker_id`、`device_id`、`role`、`resource_kind`、`instance_id`、`port_start`、`port_end`、`observed_at`、`expires_at`、`source`。声明按 Worker、设备、角色、资源类型整批替换；只有一次远程扫描完整成功时才删除该范围内已经不存在的旧声明。扫描失败不删除旧数据。`expires_at` 用于标记扫描是否新鲜；自动分配仍保守占用过期声明，直到后续完整扫描确认不存在。

`line_specs` 增加四个自动分配标记：`socks_port_auto`、`relay_port_auto`、`exit_port_auto`、`udp_ports_auto`。保存规格时由请求中的零值决定，分配完成后仍保留来源语义。

## Worker 扫描

中央提供 Agent 专用扫描计划接口，返回 Worker 获授权资源组中的全部设备、角色和跳板关系。Worker 在启动、每次心跳以及领取任务前执行扫描。

扫描读取 `/etc/NB/shards/configs/{entry,middle,exit}/*/*.conf`，解析实际 `instance_id`、角色监听端口和 Entry UDP 范围。扫描结果去重后提交 Agent 声明接口。SSH、主机密钥和凭据继续使用动态 Worker 现有实现。

## 分配与最终预占

规格保存时，自动分配器合并 `line_specs` 与 `runtime_port_claims`。手工指定端口冲突时返回包含设备、占用实例和端口范围的中文错误。

Worker 领取 `line.open` 前先刷新运行时声明。中央在任务派发事务中重新校验端口：自动端口冲突时重新选择空闲范围，同时更新 `line_specs` 和 operation request 中的 plan；手工端口冲突时拒绝派发。更新规格、operation plan 和 dispatched 状态在同一事务内完成，构成最终 CAS 预占。

## 失败与兼容

- 扫描失败：保留旧声明，不释放端口。
- 节点短暂离线：声明变为 stale，但仍参与冲突判断。
- 已确认完整扫描且配置消失：整批替换时释放声明。
- 旧规格的自动标记默认 false，保持现有端口不被静默修改。
- shard 自身的实际端口冲突校验继续作为最后门禁。

## 验收

- 未登记但实际运行的 `1083` 被 Worker 上报后，自动分配选择 `1090` 或其他真实空闲端口。
- 手工指定 `1083` 返回中文占用实例，不进入部署。
- 自动端口在派发前发生竞争时，operation plan、线路规格和客户端配置使用同一个新端口。
- 扫描失败或 Worker 短暂离线不释放已知声明。
