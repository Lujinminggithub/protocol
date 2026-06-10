# 冒烟测试说明

## 主机来源

`sample/lab-hosts.json` 中保存了当前实验环境的机器清单。

针对当前的 `广州 -> 香港 -> 哈萨克斯坦` 冒烟链路，最小路径是：

- ingress：`gz-141`
- relay：`hk-2`
- egress：`kz-1`

规范化后的拓扑定义在：

- `sample/smoke-topology.json`

## 为什么这份文件重要

这份拓扑让我们可以围绕真实实验链路持续做：

- 运行时接线检查
- UDP 冒烟测试
- 后续 AF_XDP Linux 验证

## 当前推荐方式

当前建议优先用 `UDP` 做冒烟：

1. 先用 `sample/live-bbr.conf` 或生成后的 smoke 配置启动节点
2. 基于 `sample/smoke-topology.json` 生成 ingress / relay / egress 三份配置
3. 验证 `TUN -> UDP -> process_frame -> TUN`
4. 确认链路稳定后，再切换到 `afxdp`

## 已提供的样例配置

当前已经提供现成配置：

- `sample/smoke-ingress.conf`
- `sample/smoke-relay.conf`
- `sample/smoke-egress.conf`
- `sample/smoke-brutal-ingress.conf`
- `sample/smoke-brutal-relay.conf`
- `sample/smoke-brutal-egress.conf`

## 最小编排脚本

当前最小自动化脚本是：

- `tools/smoke_orchestrator.py`

典型流程：

```powershell
python tools/smoke_orchestrator.py generate --write-sample
python tools/smoke_orchestrator.py deploy
python tools/smoke_orchestrator.py start
python tools/smoke_orchestrator.py status
```

一条命令执行完整流程：

```powershell
python tools/smoke_orchestrator.py smoke
```

Brutal 模式：

```powershell
python tools/smoke_orchestrator.py generate --profile live-brutal --write-sample
python tools/smoke_orchestrator.py smoke --profile live-brutal
```

## AF_XDP 端口配置

当前 XDP 程序里已经不再写死 `51830`。

现在方式是：

- `bpf/xdp_tunnel_kern.c` 从 `xgw_port_map` 里读取目标 UDP 端口
- `src/afxdp_linux.c` 启动时把当前运行端口写入这个 map

这样同一份 BPF 对象文件就可以复用于不同 tunnel 端口。

## 当前 smoke 脚本的能力

当前脚本支持：

- `generate`
- `deploy`
- `start`
- `status`
- `probe`
- `collect`
- `cleanup`
- `preflight`
- `smoke`

## 当前 smoke 的判定口径

现在不再只是“发了一个包就算测试过”。

当前 smoke 会做：

1. 远端环境预检
2. 旧进程、旧 TUN、旧端口强清理
3. 自动避让 relay 监听端口
4. 自动下发配置与二进制
5. 通过 `probe-send` 注入最小合法探针
6. 收集 probe 前后快照
7. 判断是否至少出现以下证据之一：
   - `runtime.recv`
   - `runtime.proc`
   - socket 状态变化

## 当前结论

在现有实验环境里：

- `live-none`：链路证据不完整
- `live-bbr`：链路完整可通
- `live-brutal`：链路证据最完整，当前是默认推荐 smoke 基线
