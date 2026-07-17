# 安装版虚机取证说明

Updated: 2026-07-14

## 适用场景

适用于：

- 虚机里只有安装后的 `PersonalSafer`
- 没有 `src` 目录
- 只需要采集运行状态、驱动状态、日志和资源文件

## 使用方法

把下面这个脚本带到虚机里执行：

- [collect-installed-evidence.ps1](E:/project/safer/scripts/collect-installed-evidence.ps1)

管理员 PowerShell 中运行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\collect-installed-evidence.ps1
```

如果安装目录不是默认位置，可以显式指定：

```powershell
.\collect-installed-evidence.ps1 -InstallDir "D:\Apps\PersonalSafer"
```

## 会采集什么

- 安装目录路径
- `PersonalSafer.exe` 信息
- `resources\driver`
- `resources\native`
- 安装目录 `log`
- `%APPDATA%\PersonalSafer\log`
- `%APPDATA%\PersonalSafer\mitm`
- `%APPDATA%\Electron\log`
- `%APPDATA%\Electron\mitm`
- `fltmc filters`
- `fltmc instances`
- `sc query/qc PersonalSafer`
- `driverquery`
- `netstat -ano`
- `Get-NetTCPConnection`
- 安装目录文件树

## 输出目录

默认输出到：

```text
%TEMP%\PersonalSafer-installed-evidence-<timestamp>\
```

## 建议回传文件

至少回传这些：

- `install-dir.txt`
- `exe-info.txt`
- `fltmc-filters.txt`
- `fltmc-instances.txt`
- `sc-query.txt`
- `sc-qc.txt`
- `netstat-ano.txt`
- `install-log\`
- `PersonalSafer-log\` 或 `Electron-log\`

## 说明

这个脚本**不会**运行源码侧验证脚本，也**不会**重建工程。

如果要验证透明导流 / WebSocket 这种需要源码侧验证脚本的能力，仍然建议在带源码的测试环境里运行：

- [transparent-redirect-validation.md](E:/project/safer/docs/transparent-redirect-validation.md)
