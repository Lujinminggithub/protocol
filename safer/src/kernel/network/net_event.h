/*
 * net_event.h - 网络事件数据结构
 */

#pragma once

#include <fltKernel.h>
#include "../common/shared_types.h"

// 网络事件记录
typedef struct _NET_EVENT_RECORD {
    LIST_ENTRY ListEntry;
    DLP_NET_EVENT EventData;
    LARGE_INTEGER CreateTime;
    BOOLEAN Active;
} NET_EVENT_RECORD, *PNET_EVENT_RECORD;

// 网络事件环形缓冲区（独立定义，不与文件事件缓冲区共用类型）
// 使用 KSPIN_LOCK：本缓冲区由 WFP 回调直接写入，可能运行于 DISPATCH_LEVEL，
// FAST_MUTEX 要求 IRQL <= APC_LEVEL，用在这里有蓝屏风险。
typedef struct _NET_EVENT_BUFFER {
    volatile ULONG WriteOffset;
    volatile ULONG ReadOffset;
    volatile ULONG DroppedEvents;
    ULONG BufferSize;
    UCHAR Buffer[65536];
    KSPIN_LOCK BufferLock;
    KEVENT NotEmptyEvent;
} NET_EVENT_BUFFER, *PNET_EVENT_BUFFER;

// 初始化/清理
NTSTATUS
NetEventInitialize(VOID);

VOID
NetEventCleanup(VOID);

_Must_inspect_result_
PDLP_NET_EVENT
NetEventAllocate(VOID);

VOID
NetEventFree(
    _In_opt_ PDLP_NET_EVENT Event
);

// 核心操作
NTSTATUS
NetEventEnqueue(
    _In_ PDLP_NET_EVENT Event
);

NTSTATUS
NetEventDequeue(
    _Out_ PDLP_NET_EVENT Event,
    _In_ ULONG TimeoutMs
);

NTSTATUS
NetEventDequeueBatch(
    _Out_ PDLP_NET_EVENT_BATCH Batch
);

ULONG
NetEventGetQueueLength(VOID);

ULONG
NetEventGetDroppedCount(VOID);

PKEVENT
NetEventGetWaitEvent(VOID);
