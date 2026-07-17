# PersonalSafer 构建排错指南

## 遇到的错误及修复

### 错误 1: C4819 / C2059 — 中文注释导致 MSVC 编译失败

**原因**: `native_entry.cpp` 等文件包含中文注释（UTF-8 编码），但 MSVC 默认使用 GBK (代码页 936) 解析源文件，导致 `}` 等字符被误解析。

**修复**:
- 在 `binding.gyp` 中添加 `/utf-8` 编译选项
- 或将所有 C++ 源文件改为纯英文注释

**当前状态**: 已修复 — `binding.gyp` 添加了 `"/utf-8"`

### 错误 2: Missing input files — monitor_bridge.cpp

**原因**: `binding.gyp` 引用了 `src/monitor/monitor_bridge.cpp`，但该文件不存在。

**修复**: 从 `binding.gyp` 的 `sources` 列表中移除此行。

**当前状态**: 已修复

### 错误 3: ERESOLVE — vite 版本冲突

**原因**: 运行 `npm audit fix --force` 将 vite 升级到 8.x，但 `@vitejs/plugin-vue@5.2.4` 只支持 vite 5.x/6.x。

**修复**: 
- **不要使用 `npm audit fix --force`**
- 删除 `node_modules` 和 `package-lock.json`，重新干净安装

**当前状态**: 已修复 `package.json` 中的版本锁定

### 错误 4: electron-rebuild 已弃用

**原因**: `electron-rebuild` 包已不再维护。

**修复**: 替换为 `@electron/rebuild`

**当前状态**: 已修复

---

## 正确的构建步骤

> **重要**: 如果你之前运行过 `npm audit fix --force`，必须先清理！

### 1. 清理旧的依赖（如果之前运行过 audit fix --force）

```powershell
cd src\native
Remove-Item -Recurse -Force node_modules
Remove-Item -Force package-lock.json 2>$null
npm install

cd ..\electron
Remove-Item -Recurse -Force node_modules
Remove-Item -Force package-lock.json 2>$null
npm install
```

### 2. 安装依赖（干净的）

```powershell
# 先装 native 模块
cd src\native
npm install

# 再装 electron 模块
cd ..\electron
npm install
```

> **注意**: 不要运行 `npm audit fix --force`，它会把 vite 升级到不兼容的版本。

### 3. 编译 N-API 模块

```powershell
cd ..\native
npx node-gyp rebuild
```

成功后会在 `build\Release\personal_safer.node` 生成文件。

### 4. 运行开发模式

```powershell
cd ..\electron
npm run dev
```

### 5. 编译内核驱动（可选，需要 WDK）

```powershell
cd ..\..\
.\scripts\build-kernel.ps1
```

如果没有 WDK，跳过此步。应用会使用 Mock 数据正常运行。

---

## 常见问题

### Q: 编译 N-API 时报错 "cannot find module 'napi.h'"

**A**: 确保已安装 node-addon-api：
```powershell
cd src\native
npm install
```

### Q: electron 启动时报 "Cannot find module 'electron'"

**A**: 确保在 `src\electron` 目录下安装依赖：
```powershell
cd src\electron
npm install
```

### Q: 中文注释仍然报 C4819 警告

**A**: 这是警告不是错误，不影响编译。如果想消除警告，可以将 C++ 源文件保存为 UTF-8 with BOM 编码，或在 VS 中设置项目属性 → 常规 → 字符集 → 使用 Unicode 字符集。

### Q: 如何跳过驱动直接运行？

**A**: 不需要驱动。应用在没有 `.node` 文件时会自动降级到 Mock 数据，UI 和功能完全可用。只需确保 `src\electron` 能 `npm install` + `npm run dev` 即可。
