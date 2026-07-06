# XGW 直播协议重构说明

## 目标

这次重构的目标，不是继续把 `xgw` 维持成一个通用 tunnel / control 平台，而是把它收敛成一个面向直播链路的专用协议，重点服务于：

- TikTok 直播
- 竞拍类场景
- 含有一段稳定专线的固定路径部署

这次设计遵循的第一性原则是：

1. 路径固定且显式
2. 配置尽量简单
3. 复杂度只投入在抗抖动和恢复能力上
4. 保留黑白名单和基础防护能力

## 为什么核心数据面用 C

当前最关键的热路径更适合用 C 实现：

- pacing、FEC、调度、零拷贝 IO 都是低时延敏感路径
- 后续 AF_XDP 以及内核附近能力更受益于可预测的内存布局
- 当前链路是固定的，不需要在数据面里再引入一个很重的运行时或控制框架

建议分工：

- `C`：数据面核心、协议帧、pacing、FEC、session 表
- `Go`：后续可选的管理面、编排面、运维工具

## 链路假设

当前协议围绕一条固定链路优化：

`mobile -> guangzhou ingress -> hongkong relay -> overseas egress -> tiktok`

关键判断：

- `广州 -> 香港` 是稳定的专线段
- 主要不稳定因素集中在：
  - `手机 -> 广州`
  - `香港 -> 海外`
  - `海外 -> TikTok`

因此当前协议重点优化：

- ingress 抗抖动
- 爆发丢包恢复
- relay 稳定转发
- 极简配置

## 简化后的角色模型

- `mobile`：客户端侧发送端
- `ingress`：广州入口节点
- `relay`：香港中继节点
- `egress`：海外出口节点

当前最小实现不再强依赖一个通用控制面。

## Profile 模型

当前保留三种对比模式：

- `live-none`
- `live-bbr`
- `live-brutal`

相比早期设计，当前模式只保留少量真正关键的运行参数：

- MTU
- payload size
- reorder window
- FEC 分片
- pacing 速率
- pacing 间隔
- 冗余副本数

## 保留的安全与策略能力

从 `tools/new` 保留下来的能力包括：

- 静态 IP 白名单 CIDR
- 精确域名和后缀域名白名单
- 带宽限期的动态学习 IP
- DOS 白名单 IP
- 带过期时间的黑名单
- 按 IP 的连接数与速率门控

## 配置模型简化

早期 JSON 面向固定直播链路来说太重了。

现在改成 `key=value` 的简单配置格式：

- 一台节点一份文件
- 路径一行写清
- 策略和 DOS 配置就地声明

示例：

```ini
node_name=gz-ingress-1
role=ingress
profile=live-bbr
path=phone@mobile=0.0.0.0:0,gz@ingress=106.75.143.135:51830,hk@relay=10.0.0.2:51830,us@egress=45.10.10.10:51830
allow_cidrs=17.0.0.0/8,8.8.8.8/32
allow_domains=live.tiktok.com
allow_domain_suffixes=tiktokcdn.com,byteoversea.com
whitelist_ips=106.75.31.69,106.75.143.135,106.75.141.139
```

## 当前实现进展

当前目录里已经具备一个可编译的 C 版本骨架，覆盖：

- 协议 / profile 模型
- 简化配置解析
- 白名单 / 黑名单 / DOS 保护

之后又逐步补上了：

1. UDP 收发抽象
2. session 与 replay window
3. 分片与 XOR FEC 恢复
4. 数据面主循环
5. Linux TUN
6. AF_XDP Linux 骨架和 BPF/XSK map 接入

## 当前数据面能力

当前已经实现：

- UDP socket 抽象：`include/xgw_transport.h`、`src/transport_udp.c`
- session 表与 replay window：`include/xgw_session.h`、`src/session.c`
- 帧头编解码：`src/protocol.c`
- 分片与 XOR FEC：`include/xgw_frame.h`、`src/frame.c`
- 数据面拼装与处理：`include/xgw_dataplane.h`、`src/dataplane.c`

CLI 自测目前可以验证：

- 大包拆分成多帧
- 故意丢失一帧后通过 FEC 恢复
- replay window 正确拒绝重复序号

运行：

```powershell
.\xgw-clang.exe selftest sample\live-bbr.conf
```

预期信号：

- `selftest.frames=2`
- `selftest.fec=...`
- `ok=fec-recovered`
- `selftest.replay_accept_1=1`
- `selftest.replay_accept_2=0`

## TUN 与 AF_XDP 状态

当前接口已经稳定下来：

- `include/xgw_tun.h`
- `include/xgw_afxdp.h`

当前状态：

- `src/tun_linux.c` 已经支持 Linux TUN 的 open/read/write/close
- `src/runtime.c` 已经把主循环串起来：
  - `TUN -> build_outbound -> UDP/AF_XDP`
  - `UDP/AF_XDP -> process_frame -> TUN`
- `src/afxdp_linux.c` 已经作为 Linux AF_XDP 接入点落地
- `bpf/xdp_tunnel_kern.c` 已经本地化，并通过 `xsks_map` 与 `xgw_port_map` 支持重定向和动态端口

在当前 Windows 环境里：

- TUN 和 AF_XDP 仍然只是代码可编译，不可直接执行
- 运行时结构与主要代码路径已经完成验证
- 目前真正完整烟测通过的仍然是 `UDP` 路径

运行形态：

```powershell
.\xgw-clang.exe run sample\live-bbr.conf
```

Linux 配置里当前重点字段包括：

- `transport=udp|afxdp`
- `hop_name=<path 中的 hop 名>`
- `listen_host=0.0.0.0`
- `tun_name=xgw0`
- `tun_addr=10.0.0.1/24`
- `device=eth0`
- `queue_id=0`
- `mtu_profile=<profile 名>`
- `payload_profile=<profile 名>`

AF_XDP 在 Linux 上的运行前提：

- 支持 BPF target 的 `clang`
- `bpftool`
- `iproute2`
- 已挂载的 `/sys/fs/bpf`
- 足够的权限，用于加载 XDP 和绑定 AF_XDP socket

## 下一步建议

Linux 方向后续优先级建议是：

1. 把 `src/afxdp_linux.c` 从“代码接好”推进到“真实链路烟测稳定通过”
2. 补齐结构化指标，而不是只看日志
3. 让 session 身份、上下游角色、路径状态真正产品化
4. 在当前固定链路的基础上，引入真实直播流量验证，而不是只靠合成 probe
