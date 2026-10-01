# 控制面统一构建、分级分发与验证调优一体化设计

## 目标

将 Node 构建统一放在控制面，按“控制面 -> Entry -> Relay -> Exit”分发二进制；将线路验证和协议调优合并为一个服务端任务，并在不破坏现有业务流程、旧线路和旧 API 的前提下实现受控并行。

## 不变约束

- NB wire format 不变，现有线上客户端保持兼容。
- 开线、停用、删除、强制删除、回滚、升级、白名单同步和端口声明的业务语义不变。
- 真实 Node 二进制切换继续受 active-session drain 门禁保护。
- 同一线路不能同时执行两个变更任务。
- 共享任一实际设备角色的线路不能并行执行部署、主动验证或 profile 提交。
- 已存在的 `line.validate`、`line.tune` API 和历史任务记录继续可读、可执行。
- 失败操作只回滚本次实际修改的角色和配置，不改变其他线路。

## 构建架构

控制面是唯一 Node 构建环境。构建工作目录固定为 `/opt/nb-controlplane/data/build`，位于 worker systemd 的可写目录内，不使用线路清单中的 `build_host`。控制面构建依赖在发布前预检：`gcc`、`g++`、`make`、`cmake`、`pkg-config` 和 `libssl-dev`。

源码上传构建、开线自动构建和手工 Node Release 构建全部调用同一本地构建入口。构建产物继续生成 release manifest、二进制 SHA256、源码输入摘要和版本信息。相同构建输入优先复用现有 release。

## 分发架构

固定分发路径：

```text
Control Plane --upload--> Entry --transfer--> Relay --transfer--> Exit
```

- 控制面只向 Entry 上传二进制。
- Entry 向 Relay 传输；Relay 向 Exit 传输。
- 节点间传输地址按 `private_ip`、`jump_target_host`、`host` 的顺序选择。
- 私网地址不可达时才回退公网地址。
- 每一跳保留断点续传、临时文件、SHA256 校验、原子发布和临时 SSH key 清理。
- 不自动使用控制面直传 Relay/Exit，也不自动使用 Entry 直传 Exit。
- 上传顺序严格为 Entry、Relay、Exit；后一级只有在前一级校验成功后开始。

Entry/Relay 成为分发链路依赖是有意设计。任一中间节点不可用时任务进入失败或等待重试，不静默切换到不同网络路径。

## 验证并调优任务

新增服务端任务类型 `line.optimize`，Web 常规操作只显示“验证并调优”。原 `line.validate` 和 `line.tune` 保留兼容，不从数据库迁移或删除。

`line.optimize` 执行顺序：

1. 校验线路状态、部署 ID、端口声明和执行器能力。
2. 读取与当前拓扑、Node release、套餐速率、profile 和探测策略匹配的缓存证据。
3. 缓存无效时执行一次主动验证；缓存有效时直接复用。
4. 验证未通过时任务失败，不生成或提交调优 profile。
5. 由验证证据生成 transport profile。
6. Entry、Relay、Exit 执行 prepare；任一失败则 abort 已 prepare 的角色。
7. 按 Exit、Relay、Entry 顺序 commit。
8. 三节点执行 generation 和 fingerprint readback。
9. 任一 commit/readback 失败时按已有 generation 执行回滚。
10. 成功后更新线路 profile 和 transport generation。

任务结果同时保存 validation evidence、transport profile、rollout 状态和 generation，前端只需查看一个任务。

## 并发与资源锁

worker 维持有界并发池，默认并发数为 4。

- 实际 Entry/Relay/Exit 设备均不相同的线路任务可以并行。
- 共享任一 `device_id + role` 的线路变更和主动验证任务串行。
- 同一线路的所有任务串行。
- 主动验证按实际路径设备加锁，避免共享 Entry/Relay/Exit 的压测互相污染。
- Node 构建使用全局构建锁，同时只允许一个构建任务。
- 只读快照、心跳和运行时端口采集不占用线路变更锁。
- 数据库继续禁止同一线路存在多个 queued/dispatched/running 操作。

`resource_group` 继续用于执行器授权、计划路由和历史端口命名空间，不作为唯一并发冲突依据，也不迁移历史值。锁必须在进程内和操作状态层面一致；worker 重启后依赖中央操作状态和幂等 checkpoint 恢复，不假设内存锁仍存在。

## 状态和恢复

`line.optimize` 使用阶段 checkpoint：`validation`、`profile-generated`、`prepared`、`committed`、`readback`。重试时已验证且缓存 key 匹配的证据不重复跑 90 秒探测；已提交但未完成 readback 的任务先查询远端 generation 再决定继续或回滚。

Node 分发分别记录 Entry、Relay、Exit 的 staged/verified 状态。重试从第一个未完成角色继续，不覆盖已经校验成功的不可变 release。

## 兼容性

- schema 2 控制面调优模型继续向旧 Node 下发 schema 1 wire profile；Node 升级完成前不下发 schema 2 专属字段。
- 历史 `line.validate` 成功结果仍可被旧 `line.tune` 使用。
- 新 `line.optimize` 不修改历史任务种类和结果结构。
- 执行器若同时支持 `line.validate` 和 `line.tune`，控制面可派生宣告 `line.optimize` 能力；旧 worker 不宣告该能力，按钮禁用并显示升级原因。
- 静态线路和动态线路使用各自原有状态目录，不迁移客户端配置路径。

## 影响面与回归覆盖

必须覆盖以下测试：

1. 控制面本地构建成功、依赖缺失、构建输入变化和产物复用。
2. 控制面只上传 Entry，Entry->Relay、Relay->Exit 顺序和私网优先/公网回退。
3. 每一跳断点续传、hash 冲突、连接失败和临时授权清理。
4. 三节点部署失败后的逆序回滚和 mutable state 恢复。
5. `line.optimize` 缓存命中、缓存失效、验证失败、prepare 失败、commit 失败、readback 失败和成功路径。
6. 同资源组串行、不同资源组并行、同线路互斥和全局构建互斥。
7. 旧 `line.validate`、`line.tune`、`line.open`、`line.upgrade`、`line.rollback`、`line.disable` 行为不变。
8. 线路删除、不可用节点强制删除、端口声明、白名单维护和历史客户端配置同步不回归。
9. 旧 Node schema 1 profile 和新 Node schema 2 能力兼容。
10. Web 按钮、任务文案、任务详情、失败原因和刷新恢复。

上线验收包含 Go 全量测试、worker race test、Python P0 部署测试、控制面本地构建 smoke、同组三节点真实开线重试和一条真实 `line.optimize`。

## 发布和回滚

先部署控制面源码和 worker，再部署 Web。上线前备份当前 `nb-web`、`nb-web-worker` 和 worker registry。出现回归时恢复二进制和源码快照；Node 三节点二进制不因控制面回滚自动切换。

本变更不主动升级现有 Node；构建和分发的新链路只在下一次 Node Release 或需要构建的开线任务中生效。
