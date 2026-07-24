/*
 * net_event.c - fixed-size ring buffer for network events
 */

#include "net_event.h"
#include "../common/shared_events.h"
#include "../core/protect.h"
#include "../policy/policy_engine.h"

static NET_EVENT_BUFFER gNetEventBuffer;
static NPAGED_LOOKASIDE_LIST gNetEventLookaside;
static BOOLEAN gNetEventLookasideInitialized;

#define NET_EVENT_SLOT_SIZE (sizeof(DLP_NET_EVENT))
#define NET_EVENT_POOL_TAG 'EvtN'

NTSTATUS
NetEventInitialize(VOID)
{
    RtlZeroMemory(&gNetEventBuffer, sizeof(gNetEventBuffer));
    gNetEventBuffer.BufferSize = sizeof(gNetEventBuffer.Buffer) -
        (sizeof(gNetEventBuffer.Buffer) % NET_EVENT_SLOT_SIZE);
    KeInitializeSpinLock(&gNetEventBuffer.BufferLock);
    KeInitializeEvent(&gNetEventBuffer.NotEmptyEvent, NotificationEvent, FALSE);
    ExInitializeNPagedLookasideList(
        &gNetEventLookaside,
        NULL,
        NULL,
        POOL_NX_ALLOCATION,
        sizeof(DLP_NET_EVENT),
        NET_EVENT_POOL_TAG,
        0);
    gNetEventLookasideInitialized = TRUE;

    DLP_LOG(DLP_DEBUG_INFO, "Net event buffer initialized");
    return STATUS_SUCCESS;
}

VOID
NetEventCleanup(VOID)
{
    if (gNetEventLookasideInitialized) {
        gNetEventLookasideInitialized = FALSE;
        ExDeleteNPagedLookasideList(&gNetEventLookaside);
    }
    RtlZeroMemory(&gNetEventBuffer, sizeof(gNetEventBuffer));
    DLP_LOG(DLP_DEBUG_INFO, "Net event buffer cleaned up");
}

PDLP_NET_EVENT
NetEventAllocate(VOID)
{
    PDLP_NET_EVENT event;

    if (!gNetEventLookasideInitialized) {
        return NULL;
    }

    event = (PDLP_NET_EVENT)ExAllocateFromNPagedLookasideList(&gNetEventLookaside);
    if (event != NULL) {
        RtlZeroMemory(event, sizeof(*event));
    }
    return event;
}

VOID
NetEventFree(
    _In_opt_ PDLP_NET_EVENT Event
    )
{
    if (Event != NULL && gNetEventLookasideInitialized) {
        ExFreeToNPagedLookasideList(&gNetEventLookaside, Event);
    }
}

NTSTATUS
NetEventEnqueue(
    _In_ PDLP_NET_EVENT Event
    )
{
    KIRQL oldIrql;
    ULONG nextWrite;
    ULONG writePos;
    ULONG bytesToWrite;

    if (Event == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!PolicyEngineIsAuditEnabled()) return STATUS_SUCCESS;
    if (ProtectShouldBypassDlp(Event->ProcessId)) return STATUS_SUCCESS;

    KeAcquireSpinLock(&gNetEventBuffer.BufferLock, &oldIrql);

    nextWrite = (gNetEventBuffer.WriteOffset + NET_EVENT_SLOT_SIZE) % gNetEventBuffer.BufferSize;
    if (nextWrite == gNetEventBuffer.ReadOffset) {
        InterlockedIncrement((volatile LONG*)&gNetEventBuffer.DroppedEvents);
        KeReleaseSpinLock(&gNetEventBuffer.BufferLock, oldIrql);
        return STATUS_BUFFER_OVERFLOW;
    }

    writePos = gNetEventBuffer.WriteOffset;
    bytesToWrite = min(NET_EVENT_SLOT_SIZE, gNetEventBuffer.BufferSize - writePos);
    RtlCopyMemory(&gNetEventBuffer.Buffer[writePos], Event, bytesToWrite);

    if (bytesToWrite < NET_EVENT_SLOT_SIZE) {
        RtlCopyMemory(
            gNetEventBuffer.Buffer,
            (PUCHAR)Event + bytesToWrite,
            NET_EVENT_SLOT_SIZE - bytesToWrite);
    }

    gNetEventBuffer.WriteOffset = nextWrite;
    KeSetEvent(&gNetEventBuffer.NotEmptyEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&gNetEventBuffer.BufferLock, oldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS
NetEventDequeue(
    _Out_ PDLP_NET_EVENT Event,
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

    KeAcquireSpinLock(&gNetEventBuffer.BufferLock, &oldIrql);
    if (gNetEventBuffer.ReadOffset == gNetEventBuffer.WriteOffset) {
        KeReleaseSpinLock(&gNetEventBuffer.BufferLock, oldIrql);
        return STATUS_NO_MORE_ENTRIES;
    }

    readPos = gNetEventBuffer.ReadOffset;
    bytesToRead = min(NET_EVENT_SLOT_SIZE, gNetEventBuffer.BufferSize - readPos);
    RtlCopyMemory(Event, &gNetEventBuffer.Buffer[readPos], bytesToRead);

    if (bytesToRead < NET_EVENT_SLOT_SIZE) {
        RtlCopyMemory(
            (PUCHAR)Event + bytesToRead,
            gNetEventBuffer.Buffer,
            NET_EVENT_SLOT_SIZE - bytesToRead);
    }

    gNetEventBuffer.ReadOffset = (gNetEventBuffer.ReadOffset + NET_EVENT_SLOT_SIZE) % gNetEventBuffer.BufferSize;
    if (gNetEventBuffer.ReadOffset == gNetEventBuffer.WriteOffset) {
        KeResetEvent(&gNetEventBuffer.NotEmptyEvent);
    }
    KeReleaseSpinLock(&gNetEventBuffer.BufferLock, oldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS
NetEventDequeueBatch(
    _Out_ PDLP_NET_EVENT_BATCH Batch
    )
{
    ULONG i;
    NTSTATUS status;

    if (Batch == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Batch, sizeof(*Batch));
    for (i = 0; i < PS_NET_BATCH_MAX; i++) {
        status = NetEventDequeue(&Batch->Events[i], 0);
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
NetEventGetQueueLength(VOID)
{
    return (gNetEventBuffer.WriteOffset >= gNetEventBuffer.ReadOffset)
        ? (gNetEventBuffer.WriteOffset - gNetEventBuffer.ReadOffset) / NET_EVENT_SLOT_SIZE
        : (gNetEventBuffer.BufferSize - gNetEventBuffer.ReadOffset + gNetEventBuffer.WriteOffset) / NET_EVENT_SLOT_SIZE;
}

ULONG
NetEventGetDroppedCount(VOID)
{
    return gNetEventBuffer.DroppedEvents;
}

PKEVENT
NetEventGetWaitEvent(VOID)
{
    return &gNetEventBuffer.NotEmptyEvent;
}
