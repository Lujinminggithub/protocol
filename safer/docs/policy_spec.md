# 策略引擎规范

## 概述

PersonalSafer 策略引擎负责管理 DLP 和审计模块的行为规则。

## 策略结构

```json
{
  "fileFilterEnabled": true,
  "networkFilterEnabled": true,
  "auditEnabled": true,
  "fileActions": {
    "create": "log",
    "write": "log",
    "read": "none",
    "delete": "log",
    "rename": "log"
  },
  "networkActions": {
    "http": "log",
    "https": "log",
    "ftp": "log"
  },
  "blockedDomains": ["example.com", "bad.example"],
  "blockedUrls": ["http://example.com/secret", "/internal/api"],
  "blockedFtpCommands": ["RETR", "STOR"],
  "blockedFtpPaths": ["/confidential", ".pem"],
  "blockedFtpContentPatterns": ["secret", "internal-only"],
  "processWhitelist": ["svchost.exe", "services.exe"],
  "processBlacklist": ["malware.exe"],
  "fileExtensions": [".exe", ".dll", ".docx", ".xlsx", ".pdf", ".zip"],
  "maxFileSize": 104857600,
  "alertThreshold": {
    "fileOpsPerMinute": 100,
    "networkConnsPerMinute": 50
  }
}
```

## 操作类型

| 值 | 说明 |
|----|------|
| `none` | 不监控 |
| `log` | 仅记录到审计日志 |
| `block` | 拦截操作并记录 |
| `quarantine` | 隔离文件并记录 |

## 策略同步流程

1. 用户在 UI 中修改策略
2. Electron 主进程通过 IPC 接收策略
3. N-API 模块通过 `IOCTL_PS_SET_POLICY` 发送到内核
4. 内核策略引擎更新规则表
5. 内核回调中查询策略引擎决定操作结果

## FTP 规则族

| 字段 | 匹配范围 |
|----|------|
| `blockedFtpCommands` | FTP 控制命令，例如 `RETR`、`STOR`、`APPE` |
| `blockedFtpPaths` | 控制命令参数、`LIST/NLST/MLSD` 目录项名称和传输文件路径 |
| `blockedFtpContentPatterns` | `RETR/STOR/APPE/STOU` 数据通道的明文内容，忽略大小写并支持跨 buffer 匹配 |

FTP 内容规则最大长度为 63 个字符。内核保留前一内容块末尾 63 字节，并以 512 字节为检查单元，因此规则可以跨 WFP callback 边界命中。二进制内容会记录 MIME/魔数和原始 preview，但当前内容规则仍以可见明文字符串匹配为主。
