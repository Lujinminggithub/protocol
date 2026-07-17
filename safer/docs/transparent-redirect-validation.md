# 透明导流验证

Updated: 2026-07-13

## 目标

采集以下链路的可用证据：

`浏览器/命令行请求 -> ALE_CONNECT_REDIRECT -> 本地代理恢复原始目的地 -> MITM 检查`

本文档面向 **虚拟机内的管理员 PowerShell 会话**。

## 需要采集什么

我们需要四类证据：

1. 驱动状态
- `PersonalSafer.sys` 已构建且可加载
- MiniFilter 实例存在
- native addon 可以读到 driver status

2. 导流状态
- redirect 配置已下发到内核
- 本地代理在预期端口监听
- 被导流 socket 的原始目的地反查能返回数据

3. 流量证据
- HTTP 请求确实到达代理
- HTTPS 请求确实到达代理
- WebSocket 升级后的消息确实经过代理检查
- 走的是透明导流路径，而不是仅显式代理模式

4. 日志
- 构建日志
- 验证脚本 stdout/stderr
- 运行前后的系统状态快照

## 一条命令采集

在管理员 PowerShell 窗口中运行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
cd E:\project\safer
.\scripts\collect-transparent-redirect-evidence.ps1
```

说明：

- 默认**不重建**，直接使用虚机上已部署的二进制
- 如果你确实希望在虚机里重新构建，再显式加：

```powershell
.\scripts\collect-transparent-redirect-evidence.ps1 -Rebuild
```

默认输出目录：

```text
E:\project\safer\artifacts\transparent-redirect-<timestamp>\
```

## 输出文件

证据目录会包含：

- `build-kernel.log`
- `build-native.log`
- `build-electron.log`
- `bundle-local-proxy.log`
- `validate-transparent-proxy.log`
- `validate-websocket-proxy.log`
- `driver-status-after.txt`
- `fltmc-filters.txt`
- `fltmc-instances.txt`
- `sc-query.txt`
- `sc-qc.txt`
- `driverquery.txt`
- `netstat-ano.txt`
- `Get-NetTCPConnection.txt`

## 成功判据

如果透明导流真正跑通，验证日志里应同时出现：

1. 驱动加载成功

```text
[Validate] loadResult: {"success":true ...}
```

2. 代理启动日志里导流已启用

```text
[TransparentProxy] kernel redirect enabled endpoint=127.0.0.1:8899 pid=...
```

3. 对透明导流连接，原始目的地已经被恢复

```text
[TransparentProxy] redirect mapping pid=... local=... -> example.com:443
```

4. HTTPS 请求已经进入 parser 和 handler

```text
[TransparentProxy] ingress tls ...
[TransparentProxy] https parser request ...
[TransparentProxy] request HEAD https://example.com/ transparent=yes
```

5. WebSocket 验证脚本给出场景结果

```text
[WebSocketValidate] scenario=explicit-ws
[WebSocketValidate] scenario=explicit-wss
```

## 如果失败

### A. 驱动没有加载起来

优先看：

- `validate-transparent-proxy.log`
- `sc-query.txt`
- `fltmc-filters.txt`
- `fltmc-instances.txt`

常见原因：

- PowerShell 窗口没有管理员权限
- 测试签名 / 证书信任问题
- 服务或实例注册残留

### B. 代理只在显式代理模式下工作

看是否出现：

```text
[Validate] transparent redirect unavailable ...
```

这说明用户态 MITM 是活的，但内核导流没有真正发生。

重点查看：

- `driver-status-after.txt`
- `validate-transparent-proxy.log`
- `netstat-ano.txt`

### C. 已经发生导流，但原始目的地没有恢复出来

看是否出现：

```text
[TransparentProxy] no redirect mapping ...
```

这通常指向以下几类问题：

- 内核 redirect classify 没有记录映射
- flow / local endpoint key 不匹配
- 实际运行的不是最新驱动

### D. HTTPS 到了 `CONNECT 200`，但没有进入 parser/handler

如果缺少下面这些日志：

```text
[TransparentProxy] https parser request ...
[TransparentProxy] request ... https://...
```

说明 TLS ingress 还没有干净地交给 HTTP parser。

### E. WebSocket 握手成功，但消息没有被检查

如果看到了 `101 Switching Protocols`，但没有看到：

```text
[WebSocketValidate] allow outcome: ...
[WebSocketValidate] block outcome: ...
```

说明 WebSocket 帧解析、消息重组或压缩解码还需要继续定位。

## 回传什么给我

如果你要我继续分析，至少回传：

1. 证据目录路径
2. `validate-transparent-proxy.log`
3. `validate-websocket-proxy.log`
4. `driver-status-after.txt`
5. `fltmc-filters.txt`
6. `fltmc-instances.txt`

大多数情况下，这些文件就足够进入下一轮诊断。
