/*
 * shared_events.h - 事件枚举和常量定义
 */

#pragma once

#include <fltKernel.h>

// 事件类型字符串映射
static CONST PCWSTR EventTypeName[] = {
    L"Unknown",
    L"FileCreate",
    L"FileWrite",
    L"FileRead",
    L"FileDelete",
    L"FileRename",
    L"NetworkConnect",
    L"NetworkDisconnect",
    L"HttpRequest",
    L"HttpResponse",
    L"FtpCommand",
    L"RegistryChange",
    L"SniCapture"
};

// 事件类型数量
#define EVENT_TYPE_COUNT (sizeof(EventTypeName) / sizeof(EventTypeName[0]))

// 事件缓冲区大小
#define EVENT_BUFFER_SIZE 65536
#define MAX_EVENT_QUEUE_LENGTH 1024

// 设备名称常量
#define DEVICE_LINK_NAME L"\\DosDevices\\PersonalSafer"
#define DEVICE_NAME_LWPTUN L"\\Device\\LWPTUN"

// 调试级别
typedef enum _DEBUG_LEVEL {
    DebugError = 1,
    DebugWarning,
    DebugInfo,
    DebugVerbose
} DEBUG_LEVEL;

#define DLP_DEBUG_ERROR   0x0001
#define DLP_DEBUG_WARN    0x0002
#define DLP_DEBUG_INFO    0x0004
#define DLP_DEBUG_VERBOSE 0x0008

// 日志宏
#if defined(_DEBUG) || defined(DBG)
#define DLP_LOG(level, fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, level, "[PS] " fmt "\n", ##__VA_ARGS__)
#else
#define DLP_LOG(level, fmt, ...) ((void)0)
#endif
