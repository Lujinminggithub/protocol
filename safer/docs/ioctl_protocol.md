# IOCTL 通信协议定义

## 概述

PersonalSafer 内核驱动与应用层通过 IOCTL 进行双向通信。

## 设备路径

- 设备名: `\\.\PersonalSafer`
- 符号链接: `\\DosDevices\\PersonalSafer`

## IOCTL 码定义

| 方向 | IOCTL 码 | CTL_CODE | 用途 |
|------|----------|----------|------|
| K→U | `IOCTL_PS_FILE_EVENT` | 0x800 | 文件事件上报 |
| K→U | `IOCTL_PS_NET_EVENT` | 0x801 | 网络事件上报 |
| U→K | `IOCTL_PS_DRIVER_START` | 0x802 | 启动驱动 |
| U→K | `IOCTL_PS_DRIVER_STOP` | 0x803 | 停止驱动 |
| U→K | `IOCTL_PS_SET_POLICY` | 0x804 | 设置策略 |
| U→K | `IOCTL_PS_GET_POLICY` | 0x805 | 获取策略 |
| U→K | `IOCTL_PS_DRIVER_STATUS` | 0x806 | 查询驱动状态 |
| U→K | `IOCTL_PS_HEARTBEAT` | 0x807 | 心跳检测 |

## 通信模式

- **METHOD_BUFFERED**: 所有 IOCTL 使用缓冲模式
- **驱动→用户**: 驱动填充 SystemBuffer，用户态读取
- **用户→驱动**: 用户态写入 SystemBuffer，驱动处理

## 事件数据结构

### 文件事件 (IOCTL_PS_FILE_EVENT)

```
DLP_FILE_EVENT {
    EVENT_TYPE EventType;       // 4 bytes
    LARGE_INTEGER Timestamp;    // 8 bytes
    ULONG ProcessId;            // 4 bytes
    USHORT ProcessNameLength;   // 2 bytes
    WCHAR ProcessName[260];     // 520 bytes
    USHORT FileNameLength;      // 2 bytes
    WCHAR FileName[512];        // 1024 bytes
    ULONG FileSize;             // 4 bytes
    ACTION_RESULT ActionResult; // 4 bytes
    ULONG Reserved;             // 4 bytes
}
// 总计: ~1572 bytes
```

### 网络事件 (IOCTL_PS_NET_EVENT)

```
DLP_NET_EVENT {
    EVENT_TYPE EventType;       // 4 bytes
    LARGE_INTEGER Timestamp;    // 8 bytes
    ULONG ProcessId;            // 4 bytes
    USHORT ProcessNameLength;   // 2 bytes
    WCHAR ProcessName[256];     // 512 bytes
    USHORT RemoteAddressLength; // 2 bytes
    UCHAR RemoteAddress[128];   // 128 bytes
    USHORT LocalAddressLength;  // 2 bytes
    UCHAR LocalAddress[128];    // 128 bytes
    USHORT UrlLength;           // 2 bytes
    WCHAR Url[1024];            // 2048 bytes
    USHORT SniDomainLength;     // 2 bytes
    WCHAR SniDomain[256];       // 512 bytes
    UINT8 Protocol;             // 1 byte
    UINT16 RemotePort;          // 2 bytes
    UINT16 LocalPort;           // 2 bytes
    ACTION_RESULT ActionResult; // 4 bytes
    ULONG Reserved;             // 4 bytes
}
// 总计: ~3352 bytes
```

### 驱动状态响应

```
DRIVER_STATUS {
    BOOLEAN DriverLoaded;       // 1 byte
    BOOLEAN FileFilterActive;   // 1 byte
    BOOLEAN NetworkFilterActive;// 1 byte
    ULONG LastErrorCode;        // 4 bytes
    LARGE_INTEGER StartTime;    // 8 bytes
    ULONG TotalEvents;          // 4 bytes
    ULONG BlockedEvents;        // 4 bytes
}
// 总计: 24 bytes
```

### 策略命令

```
POLICY_COMMAND {
    ULONG CommandId;            // 4 bytes
    ULONG PolicySize;           // 4 bytes
    UCHAR PolicyData[4096];     // 4096 bytes
}
// 总计: 4104 bytes
```

## 命名管道通信

对于大数据量事件（如完整 HTTP 请求体），使用命名管道传输：

- 管道名称: `\\.\pipe\PersonalSafer\Control`
- 模式: Overlapped I/O (异步)
- 协议: 每条消息前 4 字节为消息长度，之后为消息内容
