/*
 * file_event.c - fixed-size ring buffer for file events
 */

#include "file_event.h"
#include "../common/shared_events.h"
#include "../core/protect.h"
#include "../policy/policy_engine.h"

static FILE_EVENT_BUFFER gEventBuffer;
static NPAGED_LOOKASIDE_LIST gFileEventLookaside;
static BOOLEAN gFileEventLookasideInitialized;

#define FILE_EVENT_SLOT_SIZE (sizeof(DLP_FILE_EVENT))
#define FILE_EVENT_POOL_TAG 'EvtF'

NTSTATUS
FileEventInitialize(VOID)
{
    RtlZeroMemory(&gEventBuffer, sizeof(gEventBuffer));
    gEventBuffer.BufferSize = sizeof(gEventBuffer.Buffer) -
        (sizeof(gEventBuffer.Buffer) % FILE_EVENT_SLOT_SIZE);
    KeInitializeSpinLock(&gEventBuffer.BufferLock);
    KeInitializeEvent(&gEventBuffer.NotEmptyEvent, NotificationEvent, FALSE);
    ExInitializeNPagedLookasideList(
        &gFileEventLookaside,
        NULL,
        NULL,
        POOL_NX_ALLOCATION,
        sizeof(DLP_FILE_EVENT),
        FILE_EVENT_POOL_TAG,
        0);
    gFileEventLookasideInitialized = TRUE;

    DLP_LOG(DLP_DEBUG_INFO, "File event buffer initialized (size=%lu)", gEventBuffer.BufferSize);
    return STATUS_SUCCESS;
}

VOID
FileEventCleanup(VOID)
{
    if (gFileEventLookasideInitialized) {
        gFileEventLookasideInitialized = FALSE;
        ExDeleteNPagedLookasideList(&gFileEventLookaside);
    }
    RtlZeroMemory(&gEventBuffer, sizeof(gEventBuffer));
    DLP_LOG(DLP_DEBUG_INFO, "File event buffer cleaned up");
}

PDLP_FILE_EVENT
FileEventAllocate(VOID)
{
    PDLP_FILE_EVENT event;

    if (!gFileEventLookasideInitialized) {
        return NULL;
    }

    event = (PDLP_FILE_EVENT)ExAllocateFromNPagedLookasideList(&gFileEventLookaside);
    if (event != NULL) {
        RtlZeroMemory(event, sizeof(*event));
    }
    return event;
}

VOID
FileEventFree(
    _In_opt_ PDLP_FILE_EVENT Event
    )
{
    if (Event != NULL && gFileEventLookasideInitialized) {
        ExFreeToNPagedLookasideList(&gFileEventLookaside, Event);
    }
}

NTSTATUS
FileEventEnqueue(
    _In_ PDLP_FILE_EVENT Event
    )
{
    KIRQL oldIrql;
    ULONG writePos;
    ULONG bytesToWrite;

    if (Event == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!PolicyEngineIsAuditEnabled()) return STATUS_SUCCESS;
    if (ProtectShouldBypassDlp(Event->ProcessId)) return STATUS_SUCCESS;

    KeAcquireSpinLock(&gEventBuffer.BufferLock, &oldIrql);
    if (FileEventIsBufferFull()) {
        InterlockedIncrement((volatile LONG*)&gEventBuffer.DroppedEvents);
        KeReleaseSpinLock(&gEventBuffer.BufferLock, oldIrql);
        return STATUS_BUFFER_OVERFLOW;
    }

    writePos = gEventBuffer.WriteOffset;
    bytesToWrite = min(FILE_EVENT_SLOT_SIZE, gEventBuffer.BufferSize - writePos);
    RtlCopyMemory(&gEventBuffer.Buffer[writePos], Event, bytesToWrite);

    if (bytesToWrite < FILE_EVENT_SLOT_SIZE) {
        RtlCopyMemory(
            gEventBuffer.Buffer,
            (PUCHAR)Event + bytesToWrite,
            FILE_EVENT_SLOT_SIZE - bytesToWrite);
    }

    gEventBuffer.WriteOffset = (gEventBuffer.WriteOffset + FILE_EVENT_SLOT_SIZE) % gEventBuffer.BufferSize;
    KeSetEvent(&gEventBuffer.NotEmptyEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&gEventBuffer.BufferLock, oldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS
FileEventDequeue(
    _Out_ PDLP_FILE_EVENT Event,
    _In_ ULONG TimeoutMs
    )
{
    KIRQL oldIrql;
    ULONG readPos;
    ULONG bytesToRead;

    UNREFERENCED_PARAMETER(TimeoutMs);

    if (Event == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&gEventBuffer.BufferLock, &oldIrql);
    if (gEventBuffer.ReadOffset == gEventBuffer.WriteOffset) {
        KeReleaseSpinLock(&gEventBuffer.BufferLock, oldIrql);
        return STATUS_NO_MORE_ENTRIES;
    }

    readPos = gEventBuffer.ReadOffset;
    bytesToRead = min(FILE_EVENT_SLOT_SIZE, gEventBuffer.BufferSize - readPos);
    RtlCopyMemory(Event, &gEventBuffer.Buffer[readPos], bytesToRead);

    if (bytesToRead < FILE_EVENT_SLOT_SIZE) {
        RtlCopyMemory(
            (PUCHAR)Event + bytesToRead,
            gEventBuffer.Buffer,
            FILE_EVENT_SLOT_SIZE - bytesToRead);
    }

    gEventBuffer.ReadOffset = (gEventBuffer.ReadOffset + FILE_EVENT_SLOT_SIZE) % gEventBuffer.BufferSize;
    if (gEventBuffer.ReadOffset == gEventBuffer.WriteOffset) {
        KeResetEvent(&gEventBuffer.NotEmptyEvent);
    }
    KeReleaseSpinLock(&gEventBuffer.BufferLock, oldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS
FileEventDequeueBatch(
    _Out_ PDLP_FILE_EVENT_BATCH Batch
    )
{
    ULONG i;
    NTSTATUS status;

    if (Batch == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Batch, sizeof(*Batch));
    for (i = 0; i < PS_FILE_BATCH_MAX; i++) {
        status = FileEventDequeue(&Batch->Events[i], 0);
        if (!NT_SUCCESS(status)) {
            if (i == 0) {
                return status;
            }
            break;
        }
        Batch->Count++;
    }

    return Batch->Count > 0 ? STATUS_SUCCESS : STATUS_NO_MORE_ENTRIES;
}

ULONG
FileEventGetQueueLength(VOID)
{
    return (gEventBuffer.WriteOffset >= gEventBuffer.ReadOffset)
        ? (gEventBuffer.WriteOffset - gEventBuffer.ReadOffset) / FILE_EVENT_SLOT_SIZE
        : (gEventBuffer.BufferSize - gEventBuffer.ReadOffset + gEventBuffer.WriteOffset) / FILE_EVENT_SLOT_SIZE;
}

BOOLEAN
FileEventIsBufferFull(VOID)
{
    ULONG nextWrite = (gEventBuffer.WriteOffset + FILE_EVENT_SLOT_SIZE) % gEventBuffer.BufferSize;
    return nextWrite == gEventBuffer.ReadOffset;
}

ULONG
FileEventGetDroppedCount(VOID)
{
    return gEventBuffer.DroppedEvents;
}

PKEVENT
FileEventGetWaitEvent(VOID)
{
    return &gEventBuffer.NotEmptyEvent;
}
