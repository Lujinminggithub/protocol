/*
 * sni_capture.h - HTTPS SNI 捕获声明
 */

#pragma once

#include <fltKernel.h>

// SNI 捕获结果
typedef struct _SNI_CAPTURE_RESULT {
    ULONG ProcessId;
    USHORT ProcessName[256];
    USHORT DomainName[256];
    UCHAR RemoteAddress[128];
    UINT16 RemotePort;
    LARGE_INTEGER Timestamp;
} SNI_CAPTURE_RESULT, *PSNI_CAPTURE_RESULT;

// TLS ClientHello 解析
NTSTATUS
SnipCaptureParseClientHello(
    _In_reads_(packetLength) const UCHAR *packetData,
    _In_ ULONG packetLength,
    _Out_ PSNI_CAPTURE_RESULT Result
);

// 注册 SNI 捕获过滤器
NTSTATUS
SnipCaptureInitialize(VOID);

VOID
SnipCaptureCleanup(VOID);
