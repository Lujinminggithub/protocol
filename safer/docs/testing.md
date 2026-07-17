# PersonalSafer 集成测试指南

## 前提条件

1. Windows 10/11 (x64)
2. Visual Studio 2022 + WDK
3. Node.js 18+
4. 管理员权限

## 构建步骤

### 1. 安装依赖

```powershell
# 安装 Electron 依赖
cd src\electron
npm install

# 安装原生模块依赖
cd ..\..\src\native
npm install
```

### 2. 编译内核驱动

```powershell
.\scripts\build-kernel.ps1 -Configuration Release -Platform x64
```

输出: `build/kernel/x64/Release/PersonalSafer.sys`

### 3. 编译 N-API 模块

```powershell
.\scripts\build-native.ps1
```

输出: `build/native/personal_safer.node`

### 4. 打包 Electron 应用

```powershell
.\scripts\build-electron.ps1
```

输出: `dist/PersonalSafer-Setup.exe`

## 端到端测试

### 测试 1: 系统监控模块

1. 启动应用: `npm run dev`
2. 验证 Dashboard 显示 CPU/内存/网络/磁盘数据
3. 观察数据每 2 秒自动刷新
4. 切换到"趋势图表"标签页，验证 ECharts 折线图实时更新
5. 切换到"实时详情"标签页，验证柱状图和进度条显示

### 测试 2: 系统托盘

1. 点击窗口关闭按钮
2. 验证窗口隐藏，托盘图标出现
3. 双击托盘图标，验证窗口恢复
4. 右键托盘菜单，验证各选项正常工作
5. 点击"退出"，验证进程完全终止

### 测试 3: DLP 模块

1. 切换到"DLP 防护"标签页
2. 点击"加载"按钮（需要管理员权限）
3. 验证驱动状态显示
4. 修改策略设置，验证保存成功
5. 点击"卸载"，验证驱动卸载

### 测试 4: 审计模块

1. 切换到"审计日志"标签页
2. 验证统计卡片显示正确数据
3. 搜索日志，验证过滤功能
4. 清空日志，验证列表清空

## 性能测试

- 监控刷新间隔: 2 秒
- 历史记录深度: 60 个点
- 预期 CPU 占用: < 1%
- 预期内存占用: < 100MB

## 已知限制

1. 驱动模块需要管理员权限才能加载
2. 开发模式下使用 Mock 数据，真实数据需要编译 N-API 模块
3. WFP 网络过滤需要驱动签名（测试模式）

## 透明导流证据采集

在管理员权限虚机中，可直接运行：

```powershell
cd E:\project\safer
.\scripts\collect-transparent-redirect-evidence.ps1
```

详细说明：

- [transparent-redirect-validation.md](E:/project/safer/docs/transparent-redirect-validation.md)

如需单独验证 WebSocket 代理链：

```powershell
cd E:\project\safer\src\electron
.\node_modules\.bin\electron.cmd ..\..\scripts\validate-websocket-proxy.js
```
