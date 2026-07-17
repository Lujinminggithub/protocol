/*
 * process_tracker.c - process attribution cache for file/network events
 */

#include "process_tracker.h"
#include "../common/shared_events.h"
#include "../core/protect.h"

NTKERNELAPI PUCHAR NTAPI PsGetProcessImageFileName(_In_ PEPROCESS Process);

static PROCESS_TRACK_TABLE gProcessTable;
static BOOLEAN gProcessNotifyRegistered = FALSE;
static const LONGLONG kProcessTrackerTombstoneGrace = 5LL * 60LL * 1000LL * 1000LL * 10LL;

static
BOOLEAN
ProcessTrackerCopySystemName(
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
    )
{
    static const WCHAR kSystemName[] = L"System";
    USHORT copyLen;

    if (Buffer == NULL || BufferLengthInChars == 0) {
        return FALSE;
    }

    copyLen = (USHORT)(RTL_NUMBER_OF(kSystemName) - 1);
    if (copyLen >= BufferLengthInChars) {
        copyLen = (USHORT)(BufferLengthInChars - 1);
    }

    RtlCopyMemory(Buffer, kSystemName, copyLen * sizeof(WCHAR));
    Buffer[copyLen] = L'\0';
    return TRUE;
}

static
USHORT
ProcessTrackerCopyNameFromUnicodeString(
    _In_opt_ PCUNICODE_STRING Source,
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
    )
{
    USHORT srcChars;
    USHORT startIndex;
    USHORT copyLen;
    USHORT i;

    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0 ||
        Buffer == NULL || BufferLengthInChars == 0) {
        return 0;
    }

    srcChars = (USHORT)(Source->Length / sizeof(WCHAR));
    startIndex = 0;
    for (i = 0; i < srcChars; i++) {
        if (Source->Buffer[i] == L'\\' || Source->Buffer[i] == L'/') {
            startIndex = (USHORT)(i + 1);
        }
    }

    if (startIndex >= srcChars) {
        startIndex = 0;
    }

    copyLen = (USHORT)(srcChars - startIndex);
    if (copyLen >= BufferLengthInChars) {
        copyLen = (USHORT)(BufferLengthInChars - 1);
    }

    if (copyLen == 0) {
        return 0;
    }

    RtlCopyMemory(Buffer, Source->Buffer + startIndex, copyLen * sizeof(WCHAR));
    Buffer[copyLen] = L'\0';
    return copyLen;
}

static
USHORT
ProcessTrackerCopyAnsiProcessName(
    _In_ PEPROCESS Process,
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
    )
{
    PUCHAR imageFileName;
    USHORT nameLen;
    USHORT copyLen;
    USHORT i;

    if (Process == NULL || Buffer == NULL || BufferLengthInChars == 0) {
        return 0;
    }

    imageFileName = PsGetProcessImageFileName(Process);
    if (imageFileName == NULL) {
        return 0;
    }

    nameLen = (USHORT)strlen((char*)imageFileName);
    copyLen = min(nameLen, (USHORT)(BufferLengthInChars - 1));

    for (i = 0; i < copyLen; i++) {
        Buffer[i] = (WCHAR)imageFileName[i];
    }
    Buffer[copyLen] = L'\0';

    return copyLen;
}

static
USHORT
ProcessTrackerCopyNameFromLocatedImagePath(
    _In_ PEPROCESS Process,
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
    )
{
    PUNICODE_STRING imagePath = NULL;
    USHORT copied = 0;

    if (Process == NULL || Buffer == NULL || BufferLengthInChars == 0) {
        return 0;
    }

    if (NT_SUCCESS(SeLocateProcessImageName(Process, &imagePath)) &&
        imagePath != NULL) {
        copied = ProcessTrackerCopyNameFromUnicodeString(
            imagePath,
            Buffer,
            BufferLengthInChars);
        ExFreePool(imagePath);
    }

    return copied;
}

static
PPROCESS_TRACK_ENTRY
ProcessTrackerFindEntryLocked(
    _In_ ULONG ProcessId
    )
{
    PLIST_ENTRY entry;

    entry = gProcessTable.ProcessList.Flink;
    while (entry != &gProcessTable.ProcessList) {
        PPROCESS_TRACK_ENTRY trackEntry;

        trackEntry = CONTAINING_RECORD(entry, PROCESS_TRACK_ENTRY, ListEntry);
        if (trackEntry->ProcessId == ProcessId) {
            return trackEntry;
        }
        entry = entry->Flink;
    }

    return NULL;
}

static
VOID
ProcessTrackerReapExpiredEntriesLocked(
    _In_ BOOLEAN ForceOneIfFull
    )
{
    PLIST_ENTRY entry;
    LARGE_INTEGER now;

    if (!ForceOneIfFull && IsListEmpty(&gProcessTable.ProcessList)) {
        return;
    }

    KeQuerySystemTime(&now);
    entry = gProcessTable.ProcessList.Flink;
    while (entry != &gProcessTable.ProcessList) {
        PPROCESS_TRACK_ENTRY trackEntry;
        PLIST_ENTRY nextEntry;
        BOOLEAN shouldDelete = FALSE;

        trackEntry = CONTAINING_RECORD(entry, PROCESS_TRACK_ENTRY, ListEntry);
        nextEntry = entry->Flink;

        if (!trackEntry->Active) {
            if (trackEntry->ExitTime.QuadPart != 0 &&
                (now.QuadPart - trackEntry->ExitTime.QuadPart) >= kProcessTrackerTombstoneGrace) {
                shouldDelete = TRUE;
            } else if (ForceOneIfFull) {
                shouldDelete = TRUE;
            }
        }

        if (shouldDelete) {
            RemoveEntryList(&trackEntry->ListEntry);
            if (trackEntry->ProcessObject != NULL) {
                ObDereferenceObject(trackEntry->ProcessObject);
            }
            ExFreePool(trackEntry);
            if (gProcessTable.EntryCount > 0) {
                gProcessTable.EntryCount--;
            }
            if (ForceOneIfFull && gProcessTable.EntryCount < gProcessTable.MaxEntries) {
                break;
            }
        }

        entry = nextEntry;
    }
}

static
VOID
ProcessTrackerSetEntryName(
    _Inout_ PPROCESS_TRACK_ENTRY Entry,
    _In_opt_ PCUNICODE_STRING ImageFileName,
    _In_opt_ PEPROCESS Process
    )
{
    if (Entry == NULL) {
        return;
    }

    RtlZeroMemory(Entry->ProcessName, sizeof(Entry->ProcessName));
    Entry->ProcessNameLength = 0;

    if (ProcessTrackerIsSystemProcessId((HANDLE)(ULONG_PTR)Entry->ProcessId)) {
        if (ProcessTrackerCopySystemName(Entry->ProcessName, ARRAYSIZE(Entry->ProcessName))) {
            Entry->ProcessNameLength = (USHORT)wcslen(Entry->ProcessName);
        }
        return;
    }

    if (ImageFileName != NULL) {
        Entry->ProcessNameLength = ProcessTrackerCopyNameFromUnicodeString(
            ImageFileName,
            Entry->ProcessName,
            ARRAYSIZE(Entry->ProcessName));
    }

    if (Entry->ProcessNameLength == 0 && Process != NULL) {
        Entry->ProcessNameLength = ProcessTrackerCopyNameFromLocatedImagePath(
            Process,
            Entry->ProcessName,
            ARRAYSIZE(Entry->ProcessName));
    }

    if (Entry->ProcessNameLength == 0 && Process != NULL) {
        Entry->ProcessNameLength = ProcessTrackerCopyAnsiProcessName(
            Process,
            Entry->ProcessName,
            ARRAYSIZE(Entry->ProcessName));
    }
}

static
PPROCESS_TRACK_ENTRY
ProcessTrackerUpsertProcessLocked(
    _In_opt_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _In_opt_ PCUNICODE_STRING ImageFileName,
    _In_ BOOLEAN IncrementFileOps
    )
{
    ULONG pid;
    PPROCESS_TRACK_ENTRY entry;

    pid = (ULONG)(ULONG_PTR)ProcessId;
    entry = ProcessTrackerFindEntryLocked(pid);
    if (entry != NULL) {
        if (IncrementFileOps) {
            entry->FileOpCount++;
        }
        if (entry->ProcessNameLength == 0 || ImageFileName != NULL) {
            ProcessTrackerSetEntryName(entry, ImageFileName, Process);
        }
        if (entry->ProcessObject == NULL && Process != NULL) {
            ObReferenceObject(Process);
            entry->ProcessObject = Process;
        }
        entry->Active = TRUE;
        entry->ExitTime.QuadPart = 0;
        return entry;
    }

    if (gProcessTable.EntryCount >= gProcessTable.MaxEntries) {
        ProcessTrackerReapExpiredEntriesLocked(TRUE);
        if (gProcessTable.EntryCount >= gProcessTable.MaxEntries) {
            return NULL;
        }
    }

    entry = (PPROCESS_TRACK_ENTRY)ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(PROCESS_TRACK_ENTRY),
        'ktSP');
    if (entry == NULL) {
        return NULL;
    }

    entry->ProcessId = pid;
    entry->ProcessObject = NULL;
    entry->FileOpCount = IncrementFileOps ? 1 : 0;
    entry->Active = TRUE;
    entry->ExitTime.QuadPart = 0;
    KeQuerySystemTime(&entry->CreateTime);

    if (Process != NULL) {
        ObReferenceObject(Process);
        entry->ProcessObject = Process;
    }

    ProcessTrackerSetEntryName(entry, ImageFileName, Process);
    InsertTailList(&gProcessTable.ProcessList, &entry->ListEntry);
    gProcessTable.EntryCount++;

    return entry;
}

static
VOID
ProcessTrackerProcessNotifyEx(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    if (CreateInfo != NULL) {
        ProtectProcessStarted((ULONG)(ULONG_PTR)ProcessId, CreateInfo->ImageFileName);
        ExAcquireFastMutex(&gProcessTable.TableLock);
        ProcessTrackerUpsertProcessLocked(
            Process,
            ProcessId,
            CreateInfo->ImageFileName,
            FALSE);
        ExReleaseFastMutex(&gProcessTable.TableLock);
    } else {
        ProtectProcessStopped((ULONG)(ULONG_PTR)ProcessId);
        ProcessTrackerUntrackProcess(ProcessId);
    }
}

NTSTATUS
ProcessTrackerInitialize(VOID)
{
    InitializeListHead(&gProcessTable.ProcessList);
    gProcessTable.EntryCount = 0;
    gProcessTable.MaxEntries = 4096;
    ExInitializeFastMutex(&gProcessTable.TableLock);

    if (!gProcessNotifyRegistered) {
        NTSTATUS status;

        status = PsSetCreateProcessNotifyRoutineEx(ProcessTrackerProcessNotifyEx, FALSE);
        if (!NT_SUCCESS(status)) {
            DLP_LOG(DLP_DEBUG_ERROR, "Process notify registration failed: 0x%x", status);
            return status;
        }
        gProcessNotifyRegistered = TRUE;
    }

    DLP_LOG(DLP_DEBUG_INFO, "Process tracker initialized");
    return STATUS_SUCCESS;
}

VOID
ProcessTrackerCleanup(VOID)
{
    if (gProcessNotifyRegistered) {
        PsSetCreateProcessNotifyRoutineEx(ProcessTrackerProcessNotifyEx, TRUE);
        gProcessNotifyRegistered = FALSE;
    }

    ExAcquireFastMutex(&gProcessTable.TableLock);
    ProcessTrackerReapExpiredEntriesLocked(FALSE);
    while (!IsListEmpty(&gProcessTable.ProcessList)) {
        PLIST_ENTRY entry;
        PPROCESS_TRACK_ENTRY trackEntry;

        entry = RemoveHeadList(&gProcessTable.ProcessList);
        trackEntry = CONTAINING_RECORD(entry, PROCESS_TRACK_ENTRY, ListEntry);

        if (trackEntry->ProcessObject != NULL) {
            ObDereferenceObject(trackEntry->ProcessObject);
        }

        ExFreePool(trackEntry);
        if (gProcessTable.EntryCount > 0) {
            gProcessTable.EntryCount--;
        }
    }
    ExReleaseFastMutex(&gProcessTable.TableLock);

    DLP_LOG(DLP_DEBUG_INFO, "Process tracker cleaned up");
}

PPROCESS_TRACK_ENTRY
ProcessTrackerTrackProcess(
    _In_ PEPROCESS Process
    )
{
    PPROCESS_TRACK_ENTRY entry;
    HANDLE processId;

    if (Process == NULL) {
        return NULL;
    }

    processId = PsGetProcessId(Process);

    ExAcquireFastMutex(&gProcessTable.TableLock);
    entry = ProcessTrackerFindEntryLocked((ULONG)(ULONG_PTR)processId);
    if (entry != NULL) {
        entry->FileOpCount++;
        entry->Active = TRUE;
        entry->ExitTime.QuadPart = 0;
        ExReleaseFastMutex(&gProcessTable.TableLock);
        return entry;
    }
    ExReleaseFastMutex(&gProcessTable.TableLock);

    /* Existing processes predate the create callback. Resolve their image once,
       when they first enter the file path, rather than on every file operation. */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) ProtectObserveProcess(Process);

    ExAcquireFastMutex(&gProcessTable.TableLock);
    entry = ProcessTrackerUpsertProcessLocked(
        Process,
        processId,
        NULL,
        TRUE);
    ExReleaseFastMutex(&gProcessTable.TableLock);

    return entry;
}

VOID
ProcessTrackerUntrackProcess(
    _In_ HANDLE ProcessId
    )
{
    ULONG pid;

    pid = (ULONG)(ULONG_PTR)ProcessId;

    ExAcquireFastMutex(&gProcessTable.TableLock);
    {
        PPROCESS_TRACK_ENTRY trackEntry;

        trackEntry = ProcessTrackerFindEntryLocked(pid);
        if (trackEntry != NULL) {
            trackEntry->Active = FALSE;
            KeQuerySystemTime(&trackEntry->ExitTime);
            if (trackEntry->ProcessObject != NULL) {
                ObDereferenceObject(trackEntry->ProcessObject);
                trackEntry->ProcessObject = NULL;
            }
        }
    }
    ExReleaseFastMutex(&gProcessTable.TableLock);
}

USHORT
ProcessTrackerGetProcessName(
    _In_ PEPROCESS Process,
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
    )
{
    ULONG pid;
    USHORT copied = 0;

    if (Process == NULL || Buffer == NULL || BufferLengthInChars == 0) {
        return 0;
    }

    pid = (ULONG)(ULONG_PTR)PsGetProcessId(Process);
    copied = ProcessTrackerGetProcessNameById((HANDLE)(ULONG_PTR)pid, Buffer, BufferLengthInChars);
    if (copied != 0) {
        return copied;
    }

    if (ProcessTrackerIsSystemProcessId((HANDLE)(ULONG_PTR)pid)) {
        if (ProcessTrackerCopySystemName(Buffer, BufferLengthInChars)) {
            return (USHORT)wcslen(Buffer);
        }
        return 0;
    }

    return ProcessTrackerCopyAnsiProcessName(Process, Buffer, BufferLengthInChars);
}

USHORT
ProcessTrackerGetProcessNameById(
    _In_ HANDLE ProcessId,
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
    )
{
    ULONG pid;
    USHORT copyLen = 0;

    if (Buffer == NULL || BufferLengthInChars == 0) {
        return 0;
    }

    pid = (ULONG)(ULONG_PTR)ProcessId;
    Buffer[0] = L'\0';

    if (ProcessTrackerIsSystemProcessId(ProcessId)) {
        if (ProcessTrackerCopySystemName(Buffer, BufferLengthInChars)) {
            return (USHORT)wcslen(Buffer);
        }
        return 0;
    }

    ExAcquireFastMutex(&gProcessTable.TableLock);
    ProcessTrackerReapExpiredEntriesLocked(FALSE);
    {
        PPROCESS_TRACK_ENTRY trackEntry;

        trackEntry = ProcessTrackerFindEntryLocked(pid);
        if (trackEntry != NULL && trackEntry->ProcessNameLength > 0) {
            copyLen = trackEntry->ProcessNameLength;
            if (copyLen >= BufferLengthInChars) {
                copyLen = (USHORT)(BufferLengthInChars - 1);
            }
            RtlCopyMemory(Buffer, trackEntry->ProcessName, copyLen * sizeof(WCHAR));
            Buffer[copyLen] = L'\0';
        }
    }
    ExReleaseFastMutex(&gProcessTable.TableLock);

    return copyLen;
}

ULONG
ProcessTrackerGetProcessId(
    _In_ PEPROCESS Process
    )
{
    if (Process == NULL) {
        return 0;
    }

    return (ULONG)(ULONG_PTR)PsGetProcessId(Process);
}

BOOLEAN
ProcessTrackerIsSystemProcessId(
    _In_ HANDLE ProcessId
    )
{
    ULONG pid;

    pid = (ULONG)(ULONG_PTR)ProcessId;
    return (pid == 0 || pid == 4);
}
