/*
 * file_event.h - 文件事件数据结构
 */

#pragma once

#include <fltKernel.h>
#include "../common/shared_types.h"

// 文件事件记录
typedef struct _FILE_EVENT_RECORD {
    LIST_ENTRY ListEntry;
    DLP_FILE_EVENT EventData;
    LARGE_INTEGER CreateTime;
    BOOLEAN Active;
} FILE_EVENT_RECORD, *PFILE_EVENT_RECORD;

// 事件缓冲区（环形缓冲区）
// 使用 KSPIN_LOCK 而非 FAST_MUTEX：本缓冲区由 MiniFilter 回调直接写入，
// 而回调理论上可能在 PagingIo 等场景下运行于 DISPATCH_LEVEL，
// FAST_MUTEX 要求 IRQL <= APC_LEVEL，用在这里有蓝屏风险。
typedef struct _FILE_EVENT_BUFFER {
    volatile ULONG WriteOffset;
    volatile ULONG ReadOffset;
    volatile ULONG DroppedEvents;
    ULONG BufferSize;
    UCHAR Buffer[65536];
    KSPIN_LOCK BufferLock;
    KEVENT NotEmptyEvent;
} FILE_EVENT_BUFFER, *PFILE_EVENT_BUFFER;

// 初始化/清理
NTSTATUS
FileEventInitialize(VOID);

VOID
FileEventCleanup(VOID);

_Must_inspect_result_
PDLP_FILE_EVENT
FileEventAllocate(VOID);

VOID
FileEventFree(
    _In_opt_ PDLP_FILE_EVENT Event
);

// 核心操作
NTSTATUS
FileEventEnqueue(
    _In_ PDLP_FILE_EVENT Event
);

NTSTATUS
FileEventDequeue(
    _Out_ PDLP_FILE_EVENT Event,
    _In_ ULONG TimeoutMs
);

NTSTATUS
FileEventDequeueBatch(
    _Out_ PDLP_FILE_EVENT_BATCH Batch
);

ULONG
FileEventGetQueueLength(VOID);

BOOLEAN
FileEventIsBufferFull(VOID);

ULONG
FileEventGetDroppedCount(VOID);

PKEVENT
FileEventGetWaitEvent(VOID);
