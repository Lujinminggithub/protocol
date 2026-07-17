# PersonalSafer - 个人PC安全管理客户端

个人PC安全管理工具，提供系统监控、数据防泄漏(DLP)和审计功能。

## 功能模块

### 系统监控
- CPU 使用率监控（多核）
- 内存使用监控（物理/虚拟/页面文件）
- 网络流量监控（收/发速率、TCP/UDP 连接状态）
- 磁盘 IO 监控（读写速率、IOPS、队列深度）
- 注册表变更监控

### DLP（数据防泄漏）
- 文件系统过滤（MiniFilter）：文件创建/写入/删除/重命名监控
- 网络过滤（WFP）：HTTP/FTP 流量拦截，HTTPS SNI 捕获
- 进程-文件关联追踪
- 可配置防护策略（白名单/黑名单）

### 审计
- 文件操作审计日志
- 网络连接审计日志
- 注册表变更审计日志
- 日志查询与搜索

## 技术栈

| 层级 | 技术 |
|------|------|
| UI | Electron + Vue 3 + Vite + TypeScript |
| 图表 | Apache ECharts |
| 状态管理 | Pinia |
| 原生模块 | C++ N-API (node-addon-api) |
| 内核驱动 | KMDF/WDF 框架 (MiniFilter + WFP) |
| 通信 | IOCTL + 命名管道 |

## 目录结构

```
PersonalSafer/
├── src/
│   ├── kernel/        # KMDF 内核驱动 (C/C++)
│   ├── native/        # C++ N-API 原生模块
│   └── electron/      # Electron 应用 (Vue 3)
├── build/             # 构建产物
├── scripts/           # 构建脚本
└── docs/              # 设计文档
```

## 快速开始

### 环境要求
- Windows 10/11 (x64)
- Visual Studio 2022 + WDK
- Node.js 18+
- npm 或 yarn

### 安装依赖

```powershell
# 安装 Electron 依赖
cd src\electron
npm install

# 安装原生模块依赖
cd ..\..\src\native
npm install
```

### 构建

```powershell
# 构建内核驱动
.\scripts\build-kernel.ps1

# 构建 N-API 模块
.\scripts\build-native.ps1

# 构建 Electron 应用
.\scripts\build-electron.ps1
```

### 开发模式

```powershell
cd src\electron
npm run dev
```

## 核心设计

### 系统托盘
关闭时最小化到系统托盘，通过托盘菜单控制应用：
- 双击托盘图标：恢复窗口
- 右键托盘菜单：打开主窗口 / 查看状态 / 退出

### 数据流

```
内核驱动 → IOCTL/命名管道 → N-API 环形缓冲区 → Electron IPC → Vue 组件 → ECharts
```

## 许可证

MIT

## 透明导流验证

管理员虚机验证说明：

- [docs/transparent-redirect-validation.md](E:/project/safer/docs/transparent-redirect-validation.md)
- [docs/installed-vm-evidence.md](E:/project/safer/docs/installed-vm-evidence.md)

快速采集证据：

```powershell
cd E:\project\safer
.\scripts\collect-transparent-redirect-evidence.ps1
```
