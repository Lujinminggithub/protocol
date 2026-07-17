# PersonalSafer 架构设计文档

> 📌 DLP 方向的详细架构与演进方案见 [`dlp/README.md`](./dlp/README.md)（主索引）。本文档为整体概览。

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────┐
│                  Electron 渲染进程 (Vue 3)               │
│  Dashboard(ECharts) │ DLP策略面板 │ 审计日志 │ 设置      │
├─────────────────────────────────────────────────────────┤
│                  Electron 主进程                         │
│  托盘管理 │ IPC路由 │ 模块生命周期 │ 驱动管理器          │
├─────────────────────────────────────────────────────────┤
│              C++ N-API 原生模块 (.node)                  │
│  MonitorAddon │ DLPAddon(KernelComm) │ AuditAddon      │
├─────────────────────────────────────────────────────────┤
│              Windows Kernel Mode (KMDF)                  │
│  PersonalSafer.sys: MiniFilter(文件) + WFP(网络)        │
└─────────────────────────────────────────────────────────┘
```

## 2. 模块划分

### 2.1 系统监控模块
- **位置**: `src/native/src/monitor/`
- **API**: NtQuerySystemInformation + Win32 API
- **无需驱动**: 纯用户态实现，独立于 DLP 模块

### 2.2 DLP 模块
- **内核驱动**: `src/kernel/` - MiniFilter + WFP
- **N-API**: `src/native/src/dlp/` - 通信、策略、消息处理
- **UI**: `src/electron/renderer/src/components/dlp/`

### 2.3 审计模块
- **N-API**: `src/native/src/audit/` - 日志持久化
- **UI**: `src/electron/renderer/src/components/audit/`

## 3. 通信协议

### 3.1 IOCTL 通信
- `IOCTL_PS_FILE_EVENT` - 文件事件上报
- `IOCTL_PS_NET_EVENT` - 网络事件上报
- `IOCTL_PS_SET_POLICY` - 策略下发
- `IOCTL_PS_DRIVER_STATUS` - 驱动状态查询

### 3.2 命名管道
- 路径: `\\.\pipe\PersonalSafer\Control`
- 用途: 大数据量事件传输

### 3.3 事件数据结构
详见 `src/kernel/common/shared_types.h`

## 4. 关键设计决策

1. **监控与 DLP 物理隔离** - 监控模块不依赖驱动
2. **单驱动承载 MiniFilter + WFP** - 简化部署
3. **环形缓冲区 + Overlapped I/O** - 高效异步事件分发
4. **审计复用 DLP 内核** - 发布-订阅模式
