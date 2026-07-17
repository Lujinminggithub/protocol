/*
 * pipe_manager.h - 命名管道管理声明
 */

#pragma once

#include <fltKernel.h>

// 命名管道名称
#define PIPE_NAME L"\\PersonalSafer\\Control"

// 管道事件类型
typedef enum _PIPE_EVENT_TYPE {
    PipeEventFileEvent = 1,
    PipeEventNetEvent,
    PipeEventPolicyUpdate,
    PipeEventHeartbeat
} PIPE_EVENT_TYPE;

// 管道事件头
typedef struct _PIPE_EVENT_HEADER {
    PIPE_EVENT_TYPE EventType;
    ULONG EventSize;
    ULONG SequenceNumber;
    LARGE_INTEGER Timestamp;
} PIPE_EVENT_HEADER, *PPIPE_EVENT_HEADER;

// 初始化/清理
NTSTATUS
PipeManagerInitialize(VOID);

VOID
PipeManagerCleanup(VOID);

// 核心操作
NTSTATUS
PipeManagerCreate(VOID);

VOID
PipeManagerDestroy(VOID);

NTSTATUS
PipeManagerSendEvent(
    _In_ PIPE_EVENT_TYPE EventType,
    _In_reads_(eventSize) PVOID EventData,
    _In_ ULONG EventSize
);

NTSTATUS
PipeManagerReceiveEvent(
    _Out_writes_bytes_(bufferSize) PVOID Buffer,
    _In_ ULONG bufferSize,
    _Out_ PULONG bytesRead,
    _In_ ULONG TimeoutMs
);
