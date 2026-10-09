# NB 通用香港单节点模式设计

## 状态

设计阶段。本文描述新增的 `single_hk` 拓扑模式，不改变现有三跳线路的行为。

## 目标

新增一种 NB 线路模式：一台香港节点同时承担客户端入口和目标出口，数据路径为：

```text
客户端 -> 香港 NB 节点 -> 配置允许的目标服务
```

该模式用于消除广州入口、香港中继、远端出口之间的额外 RTT、队列和中继故障面，同时保留现有 SOCKS、TCP/UDP、白名单、限速、FEC、状态观测和事务回滚能力。线路不限定为 TikTok；TikTok 直播只是一个可选的严格业务策略。

## 非目标

- 不删除或重构现有 `entry -> middle -> exit` 三跳模式。
- 不把单节点模式伪装成三跳模式，也不创建无意义的本机 Middle 转发。
- 不在本次改造中改变客户端 SOCKS5 协议、认证格式或白名单语法。
- 不自动迁移正在运行的三跳线路；线路必须显式选择 `single_hk`。
- 不在香港到目标服务的本机出站路径上套用中继方向 FEC。
- 不在协议核心中硬编码 TikTok、Teko 或直播状态；这些属于可选业务策略。

## 术语与拓扑模型

控制面线路增加拓扑字段：

```text
topology_mode = "trihop" | "single_hk"
service_profile = "general" | "tiktok_live"
```

旧数据缺少该字段时按 `trihop` 解释，保证向后兼容。

`single_hk` 线路只包含一个设备节点。该节点的能力为 `entry` 和 `exit`，但不把 `entry_exit` 当成需要在全局注册的新永久角色；运行时通过拓扑模式选择合并路径，继续复用 Entry 接入和 Exit 出站的内部模块。`service_profile` 只决定白名单、FEC、状态判定和验证门槛，不改变数据路径。

`general` 是默认策略，面向普通 TCP、UDP、DNS、网页、API、音视频和其他获准目标；`tiktok_live` 是可选的严格策略，才启用 TikTok CDN/Teko、直播媒体端口和直播准入门槛。

示例线路计划：

```json
{
  "topology_mode": "single_hk",
  "service_profile": "general",
  "instance_id": "hk-single-00001_1",
  "node": {
    "device_id": "HK-xxx",
    "roles": ["entry", "exit"]
  },
  "socks_port": 1080,
  "udp_port_min": 20000,
  "udp_port_max": 21023,
  "bandwidth_mbps": 5
}
```

## 数据流

### TCP

1. 客户端连接香港节点的 SOCKS5 监听端口。
2. 香港节点完成认证、目标分类、白名单检查和租户限速。
3. 节点在本机直接建立目标 TCP 连接。
4. 上下行字节、首包、连接错误和目标响应时间统一归入单节点会话。

### UDP

1. 客户端通过 SOCKS5 UDP Associate 建立会话。
2. 香港节点在客户端到香港的 NB UDP/QUIC 方向执行队列、丢包统计和 FEC。
3. 香港节点直接向目标 UDP 地址发送和接收数据报。
4. 不建立 Entry-to-Middle 或 Middle-to-Exit 的中继连接池。

### FEC

现有客户端接口是标准 SOCKS5，客户端到香港之间没有 NB QUIC/FEC 协议。因此 `single_hk` 模式强制关闭 NB 链路 FEC，不产生 repair、recovered 或中继丢包指标，也不能声称 FEC 保护客户端公网段。目标协议自身的重传或纠错由目标协议负责。

若未来提供原生 NB 客户端，可通过新的、独立设计的客户端传输层增加端到端 FEC；该能力不属于本次实现。

## 运行时设计

### 节点启动

单节点实例仍使用命名实例目录和控制 socket：

```text
/etc/NB/instances/<instance_id>/
/run/nb-<instance_id>-entry-<worker>.ctl
/run/nb-<instance_id>-exit-<worker>.ctl
```

第一阶段由同一香港设备上的 Entry 和 Exit 两个逻辑服务提供。Entry 通过 `127.0.0.1:<exit_port>` 的 QUIC 连接本机 Exit，复用现有可靠 TCP/UDP、白名单、限速和目标出站实现。该连接不是 Middle：不能启动 Middle 服务、生成 Middle socket 或使用 Middle 端口，也不能启用 FEC。

两个逻辑服务共享同一实例配置、租户策略和发布事务，但分别保留 Entry/Exit 证书与控制 socket。公网数据路径仍只有一个香港节点。

### 端口

单节点端口分配必须在同一设备范围内做最终 CAS 预占：

- SOCKS 端口：客户端入口监听；
- UDP relay 范围：客户端 UDP 会话；
- Exit 端口：仅保留为兼容配置项时才分配，真正运行路径不监听远程 Middle；
- 不分配 Middle 端口。

若实现采用同机 Entry/Exit 两个服务，两个服务的监听集合必须在启动前检查，禁止同端口冲突。用户手工指定的冲突端口必须返回中文错误和占用实例，不得静默改写。

### 证书与密钥

- 客户端入口继续使用 Entry 证书；
- 目标出站若复用 Exit 模块，可保留 Exit 证书材料，但不对外建立 Middle 连接；
- 不生成或分发 Middle 证书；
- 单节点证书目录必须纳入实例级备份和回滚清单；
- 既有三跳实例的证书和服务不受影响。

## 控制面设计

### 计划生成

线路计划生成器按 `topology_mode` 分支：

- `trihop`：继续要求 entry、middle、exit 三个设备和三段端口；
- `single_hk`：要求一个香港设备具备 entry/exit 能力，只生成一个节点配置和客户端入口；`service_profile` 默认 `general`，只有明确指定时才使用 `tiktok_live`。

控制面必须拒绝以下非法配置：

- `single_hk` 没有节点；
- 节点不在香港区域或缺少可信主机密钥；
- 节点声明了多个不同设备；
- 线路同时要求 Middle 端口或远端 Exit；
- `service_profile` 不是 `general` 或 `tiktok_live`；
- 端口、UDP 范围与同设备现有运行实例冲突。

### 部署事务

单节点开线顺序：

```text
生成计划
-> 读取运行时端口声明
-> 远程扫描最终端口占用
-> CAS 预占
-> 生成/备份实例安全材料
-> 上传单节点配置和二进制
-> 停止旧同实例配置（若存在）
-> 启动 Entry/Exit 逻辑服务
-> 检查控制 socket、systemd、二进制哈希
-> 执行 TCP/UDP 冒烟
-> 写入 active 状态
```

### 失败清理

任何阶段失败都必须执行实例级清理：

1. 停止本次实例的 Entry/Exit 服务；
2. 删除本次实例配置并 reload；
3. 等待两个控制 socket 消失；
4. 回读端口监听和 Worker 扫描结果；
5. 释放本次事务的端口声明；
6. 恢复原二进制、unit、证书和策略文件；
7. 线路保持 `draft` 或 `failed`，不得伪标记为 `active`。

节点不可达时不能释放运行时端口声明，必须保留 orphan 标记，等待 Worker 确认清理。

### 删除线路

删除 `single_hk` 线路必须复用现有两阶段删除语义：先创建 `line.cleanup`/`line.disable` 任务，清理香港节点上的所有实例配置、服务和 socket，确认端口释放后再删除控制面线路记录。

## 业务策略

### `general`

- 允许通用域名、IP 和端口白名单；
- 未配置静态白名单或远程 SRS 时，默认允许已认证客户端访问通用 IPv4、常规域名和全部端口；生产线路应按用途显式收紧规则；
- 不要求持续 5Mbps 媒体吞吐；
- 不因低流量或没有媒体数据而判红；
- NB 链路 FEC 固定关闭；
- 50000–50030 可以作为线路策略中的默认端口范围，但不能在协议核心中绑定为 TikTok 专属含义；
- 健康验证以连接建立、目标首包、持续收发和错误率为主。

### `tiktok_live`

- 可加载 TikTok CDN、Teko 和直播媒体端口策略；
- 对 50000–50030 执行明确的动态媒体端口放行；
- 额外检查直播首包、上下行吞吐、媒体持续流量和观看端卡顿；
- 只有该策略要求 5Mbps 资格测试和长时生产观察；
- 严格策略失败不得改变 `general` 线路的可用性判定。

## 状态与可观测性

单节点快照至少包含：

- `topology_mode`；
- Entry worker 健康和会话数；
- Exit worker 健康和目标连接数；
- 客户端 TCP/UDP 会话数和本机 Entry-to-Exit 回环延迟；
- 目标连接耗时、首包耗时、目标错误；
- 上下行吞吐、队列年龄、限速状态；
- `middle_sessions=0` 或不输出 Middle 字段，不能把不存在的 Middle 当成 unhealthy。

状态判断建议：

- `general` 绿色：客户端接入成功、目标连接/首包成功且存在正常收发；
- `general` 黄色：接入成功但目标连接、首包、错误率或吞吐处于退化阈值；
- `general` 红色：接入失败、目标连接失败、持续收发中断或控制会话断开；
- `tiktok_live` 在上述基础上增加媒体持续流量和直播质量门槛。

所有状态事件必须带 `topology_mode` 和 `instance_id`，便于与旧三跳日志区分。

## 兼容性

- 旧线路和旧配置默认 `trihop`，不改变现有部署命令参数含义。
- 旧 Worker 读取新计划时若不认识 `topology_mode`，必须拒绝执行并返回明确中文错误，不得按三跳误部署。
- 客户端 URL 格式保持 `socks5://user:password@host:port`。
- 白名单中 `50000–50030` 是否默认放行由业务策略决定；协议核心不把它解释为 TikTok 专属端口。
- 现有限速、媒体优先和端口声明功能继续复用；FEC 在 `single_hk` 中明确关闭。

## 测试策略

### 单元测试

- 计划解析和旧配置默认值；
- 单节点只接受一个设备；
- 不生成 Middle 端口、证书和连接池；
- 同设备端口冲突；
- 单节点白名单和 50000–50030 放行；
- `general` 与 `tiktok_live` 策略的配置隔离；
- 单节点状态机和指标归属；
- 失败清理和 orphan 声明。

### 集成测试

- 香港单节点 TCP CONNECT 和首包；
- UDP 50000、50017、50030；
- 通用 TCP、UDP、DNS 和自定义目标；
- `general` 低流量会话不误判红色；
- `tiktok_live` 的 TikTok CDN/Teko、媒体端口和直播门槛；
- 单节点 Entry-to-Exit 回环链路不启用 FEC；
- 目标 DNS、TCP 建连超时和 UDP 无响应；
- 节点重启、控制 socket 消失、端口被占用；
- 事务失败回滚后二次开线；
- 旧三跳线路全量回归。

### 生产灰度

先创建独立的通用测试线路，不替换既有三跳线路：

```text
hk-single-00001
```

阶段顺序：

1. 空闲启动和健康检查；
2. TCP/UDP 冒烟；
3. 通用 TCP/UDP/DNS、低流量和上下行观察；
4. 30 分钟通用稳定性观察；
5. 如需验证直播，再创建 `service_profile=tiktok_live` 的独立测试线；
6. 对比首包、状态、吞吐、CPU、内存、FEC 和观看端卡顿；
7. 满足对应策略门槛后再考虑替换线路。

## 回滚

回滚粒度为线路实例，而不是整台香港机器：

- 单节点失败：恢复该实例的前一版二进制和配置；
- 单节点模式不可用：线路回到 `draft/failed`，不影响其他三跳线路；
- 需要恢复旧业务时，使用独立三跳线路重新开线，不把单节点配置转换成三跳配置；
- 所有回滚必须有 deployment ID、端口声明、控制 socket 和健康检查证据。

## 分支

实现分支：

```text
hk-single-node
```

设计文档提交后，下一步才编写逐任务实现计划。计划将拆分为协议运行时、控制面计划/部署、观测指标、测试与灰度五个可独立验证的部分。
