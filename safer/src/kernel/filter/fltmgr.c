/*
 * fltmgr.c - MiniFilter file operation callbacks
 */

#include "fltmgr.h"
#include <ntstrsafe.h>
#include "process_tracker.h"
#include "file_event.h"
#include "../driver.h"
#include "../policy/policy_engine.h"
#include "../core/protect.h"

#define PS_COPY_CHUNK_SIZE (64 * 1024)

typedef struct _PS_QUARANTINE_MAP_ENTRY {
    LIST_ENTRY ListEntry;
    LARGE_INTEGER UpdateTime;
    USHORT OriginalPathLength;
    WCHAR OriginalPath[512];
    USHORT QuarantinePathLength;
    WCHAR QuarantinePath[512];
} PS_QUARANTINE_MAP_ENTRY, *PPS_QUARANTINE_MAP_ENTRY;

typedef struct _PS_SYNTHETIC_CONTEXT_SCRATCH {
    PS_QUARANTINE_SETTINGS Settings;
    WCHAR QuarantinePath[640];
} PS_SYNTHETIC_CONTEXT_SCRATCH, *PPS_SYNTHETIC_CONTEXT_SCRATCH;

typedef struct _PS_CREATE_REDIRECT_SCRATCH {
    PS_QUARANTINE_SETTINGS Settings;
    WCHAR QuarantinePath[640];
    WCHAR OriginalPath[512];
} PS_CREATE_REDIRECT_SCRATCH, *PPS_CREATE_REDIRECT_SCRATCH;

typedef struct _PS_DIRECTORY_POST_SCRATCH {
    WCHAR OriginalPath[640];
    WCHAR QuarantinePath[640];
    FILE_NETWORK_OPEN_INFORMATION NetworkInfo;
} PS_DIRECTORY_POST_SCRATCH, *PPS_DIRECTORY_POST_SCRATCH;

static FAST_MUTEX gQuarantineMapLock;
static LIST_ENTRY gQuarantineMapList;
static BOOLEAN gQuarantineMapInitialized = FALSE;

static
VOID
SetEventOriginalPath(
    _Inout_ DLP_FILE_EVENT* Event,
    _In_opt_z_ PCWSTR OriginalPath
    );

static
VOID
SetEventQuarantinePath(
    _Inout_ DLP_FILE_EVENT* Event,
    _In_opt_z_ PCWSTR QuarantinePath
    );

static
BOOLEAN
ShouldBypassFileOperation(_Inout_ PFLT_CALLBACK_DATA Data)
{
    PEPROCESS process;
    ULONG processId;

    process = FltGetRequestorProcess(Data);
    if (process == NULL) return FALSE;
    processId = HandleToULong(PsGetProcessId(process));
    if (ProtectShouldBypassDlp(processId)) return TRUE;

    ProcessTrackerTrackProcess(process);
    return ProtectShouldBypassDlp(processId);
}

VOID
QuarantineWriteContextCleanup(
    _In_ PFLT_CONTEXT Context,
    _In_ FLT_CONTEXT_TYPE ContextType
    )
{
    PPS_QUARANTINE_WRITE_CONTEXT writeContext;

    UNREFERENCED_PARAMETER(ContextType);

    writeContext = (PPS_QUARANTINE_WRITE_CONTEXT)Context;
    if (writeContext->QuarantineHandle != NULL) {
        FltClose(writeContext->QuarantineHandle);
        writeContext->QuarantineHandle = NULL;
    }
    if (writeContext->QuarantineFileObject != NULL) {
        ObDereferenceObject(writeContext->QuarantineFileObject);
        writeContext->QuarantineFileObject = NULL;
    }
}

static
VOID
EnsureQuarantineMapInitialized(VOID)
{
    if (!gQuarantineMapInitialized) {
        ExInitializeFastMutex(&gQuarantineMapLock);
        InitializeListHead(&gQuarantineMapList);
        gQuarantineMapInitialized = TRUE;
    }
}

static
PPS_QUARANTINE_MAP_ENTRY
FindQuarantineMapEntryLocked(
    _In_ PCWSTR OriginalPath
    )
{
    PLIST_ENTRY entry;

    if (OriginalPath == NULL) {
        return NULL;
    }

    entry = gQuarantineMapList.Flink;
    while (entry != &gQuarantineMapList) {
        PPS_QUARANTINE_MAP_ENTRY mapEntry;

        mapEntry = CONTAINING_RECORD(entry, PS_QUARANTINE_MAP_ENTRY, ListEntry);
        if (_wcsicmp(mapEntry->OriginalPath, OriginalPath) == 0) {
            return mapEntry;
        }
        entry = entry->Flink;
    }

    return NULL;
}

static
VOID
RegisterQuarantineMapping(
    _In_ PCWSTR OriginalPath,
    _In_ PCWSTR QuarantinePath
    )
{
    PPS_QUARANTINE_MAP_ENTRY entry;
    LARGE_INTEGER now;

    if (OriginalPath == NULL || QuarantinePath == NULL || OriginalPath[0] == L'\0' || QuarantinePath[0] == L'\0') {
        return;
    }

    EnsureQuarantineMapInitialized();
    ExAcquireFastMutex(&gQuarantineMapLock);
    entry = FindQuarantineMapEntryLocked(OriginalPath);
    if (entry == NULL) {
        entry = (PPS_QUARANTINE_MAP_ENTRY)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*entry), 'mqSP');
        if (entry != NULL) {
            RtlZeroMemory(entry, sizeof(*entry));
            InsertTailList(&gQuarantineMapList, &entry->ListEntry);
        }
    }

    if (entry != NULL) {
        entry->OriginalPathLength = (USHORT)min(wcslen(OriginalPath), ARRAYSIZE(entry->OriginalPath) - 1);
        entry->QuarantinePathLength = (USHORT)min(wcslen(QuarantinePath), ARRAYSIZE(entry->QuarantinePath) - 1);
        RtlCopyMemory(entry->OriginalPath, OriginalPath, entry->OriginalPathLength * sizeof(WCHAR));
        entry->OriginalPath[entry->OriginalPathLength] = L'\0';
        RtlCopyMemory(entry->QuarantinePath, QuarantinePath, entry->QuarantinePathLength * sizeof(WCHAR));
        entry->QuarantinePath[entry->QuarantinePathLength] = L'\0';
        KeQuerySystemTime(&now);
        entry->UpdateTime = now;
    }
    ExReleaseFastMutex(&gQuarantineMapLock);
}

static
BOOLEAN
LookupQuarantineMapping(
    _In_ PCWSTR OriginalPath,
    _Out_writes_(DestinationCch) PWCHAR QuarantinePath,
    _In_ SIZE_T DestinationCch
    )
{
    PPS_QUARANTINE_MAP_ENTRY entry;
    BOOLEAN found = FALSE;

    if (!gQuarantineMapInitialized || OriginalPath == NULL || QuarantinePath == NULL || DestinationCch == 0) {
        return FALSE;
    }

    QuarantinePath[0] = L'\0';
    ExAcquireFastMutex(&gQuarantineMapLock);
    entry = FindQuarantineMapEntryLocked(OriginalPath);
    if (entry != NULL) {
        RtlStringCchCopyW(QuarantinePath, DestinationCch, entry->QuarantinePath);
        found = TRUE;
    }
    ExReleaseFastMutex(&gQuarantineMapLock);
    return found;
}

VOID
FltMgrCleanup(VOID)
{
    if (!gQuarantineMapInitialized) {
        return;
    }

    ExAcquireFastMutex(&gQuarantineMapLock);
    while (!IsListEmpty(&gQuarantineMapList)) {
        PLIST_ENTRY entry = RemoveHeadList(&gQuarantineMapList);
        PPS_QUARANTINE_MAP_ENTRY mapEntry = CONTAINING_RECORD(entry, PS_QUARANTINE_MAP_ENTRY, ListEntry);
        ExFreePool(mapEntry);
    }
    ExReleaseFastMutex(&gQuarantineMapLock);
}

static
BOOLEAN
BuildFileEvent(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ DLP_EVENT_TYPE EventType,
    _Out_ DLP_FILE_EVENT* Event,
    _Outptr_ PFLT_FILE_NAME_INFORMATION* NameInfoOut
    )
{
    PEPROCESS process;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    NTSTATUS status;

    if (Event == NULL || NameInfoOut == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return FALSE;
    }

    process = FltGetRequestorProcess(Data);
    if (process == NULL) {
        return FALSE;
    }

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    FltParseFileNameInformation(nameInfo);
    RtlZeroMemory(Event, sizeof(*Event));
    Event->EventType = EventType;
    Event->ProcessId = (ULONG)(ULONG_PTR)PsGetProcessId(process);
    KeQuerySystemTime(&Event->Timestamp);
    Event->ProcessNameLength = ProcessTrackerGetProcessName(
        process,
        Event->ProcessName,
        ARRAYSIZE(Event->ProcessName));

    if (nameInfo->Name.Buffer != NULL && nameInfo->Name.Length > 0) {
        Event->FileNameLength = (USHORT)min(
            nameInfo->Name.Length / sizeof(WCHAR),
            ARRAYSIZE(Event->FileName) - 1);
        RtlCopyMemory(
            Event->FileName,
            nameInfo->Name.Buffer,
            Event->FileNameLength * sizeof(WCHAR));
        Event->FileName[Event->FileNameLength] = L'\0';
    }
    SetEventOriginalPath(Event, Event->FileName);
    SetEventQuarantinePath(Event, NULL);

    if (EventType == EventFileWrite) {
        Event->FileSize = Data->Iopb->Parameters.Write.Length;
    }

    *NameInfoOut = nameInfo;
    return TRUE;
}

static
VOID
SetEventOriginalPath(
    _Inout_ DLP_FILE_EVENT* Event,
    _In_opt_z_ PCWSTR OriginalPath
    )
{
    if (Event == NULL) {
        return;
    }

    Event->OriginalFileNameLength = 0;
    Event->OriginalFileName[0] = L'\0';
    if (OriginalPath == NULL || OriginalPath[0] == L'\0') {
        return;
    }

    Event->OriginalFileNameLength = (USHORT)min(wcslen(OriginalPath), ARRAYSIZE(Event->OriginalFileName) - 1);
    RtlCopyMemory(Event->OriginalFileName, OriginalPath, Event->OriginalFileNameLength * sizeof(WCHAR));
    Event->OriginalFileName[Event->OriginalFileNameLength] = L'\0';
}

static
VOID
SetEventQuarantinePath(
    _Inout_ DLP_FILE_EVENT* Event,
    _In_opt_z_ PCWSTR QuarantinePath
    )
{
    if (Event == NULL) {
        return;
    }

    Event->QuarantineFileNameLength = 0;
    Event->QuarantineFileName[0] = L'\0';
    if (QuarantinePath == NULL || QuarantinePath[0] == L'\0') {
        return;
    }

    Event->QuarantineFileNameLength = (USHORT)min(wcslen(QuarantinePath), ARRAYSIZE(Event->QuarantineFileName) - 1);
    RtlCopyMemory(Event->QuarantineFileName, QuarantinePath, Event->QuarantineFileNameLength * sizeof(WCHAR));
    Event->QuarantineFileName[Event->QuarantineFileNameLength] = L'\0';
}

static
VOID
InitializeContextOnlyFileEvent(
    _Out_ DLP_FILE_EVENT* Event,
    _In_ PPS_QUARANTINE_WRITE_CONTEXT Context
    )
{
    if (Event == NULL || Context == NULL) {
        return;
    }

    RtlZeroMemory(Event, sizeof(*Event));
    Event->EventType = EventFileWrite;
    Event->ProcessId = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    KeQuerySystemTime(&Event->Timestamp);
    SetEventOriginalPath(Event, Context->OriginalPath);
    SetEventQuarantinePath(Event, Context->QuarantinePath);
}

static
BOOLEAN
IsCreateDispositionRedirectable(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    ULONG disposition = (Data->Iopb->Parameters.Create.Options >> 24) & 0xFF;

    switch (disposition) {
        case FILE_CREATE:
        case FILE_SUPERSEDE:
        case FILE_OPEN_IF:
        case FILE_OVERWRITE:
        case FILE_OVERWRITE_IF:
            return TRUE;
        default:
            return FALSE;
    }
}

static
BOOLEAN
ShouldInheritAppendOnlyAccess(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    ACCESS_MASK desiredAccess;

    if (Data == NULL ||
        Data->Iopb == NULL ||
        Data->Iopb->Parameters.Create.SecurityContext == NULL) {
        return FALSE;
    }

    desiredAccess = Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
    return FlagOn(desiredAccess, FILE_APPEND_DATA) && !FlagOn(desiredAccess, FILE_WRITE_DATA);
}

static
VOID
SanitizeComponent(
    _In_ const UNICODE_STRING* Component,
    _Out_writes_(OutputCch) PWSTR Output,
    _In_ SIZE_T OutputCch
    )
{
    SIZE_T i;
    SIZE_T out = 0;

    if (Output == NULL || OutputCch == 0) {
        return;
    }

    Output[0] = L'\0';
    if (Component == NULL || Component->Buffer == NULL || Component->Length == 0) {
        RtlStringCchCopyW(Output, OutputCch, L"quarantined.bin");
        return;
    }

    for (i = 0; i < Component->Length / sizeof(WCHAR) && out < OutputCch - 1; i++) {
        WCHAR ch = Component->Buffer[i];
        switch (ch) {
            case L'\\':
            case L'/':
            case L':':
            case L'*':
            case L'?':
            case L'"':
            case L'<':
            case L'>':
            case L'|':
                Output[out++] = L'_';
                break;
            default:
                Output[out++] = ch;
                break;
        }
    }

    Output[out] = L'\0';
    if (out == 0) {
        RtlStringCchCopyW(Output, OutputCch, L"quarantined.bin");
    }
}

static
BOOLEAN
BuildQuarantinePath(
    _In_ const PS_QUARANTINE_SETTINGS* Settings,
    _In_ const UNICODE_STRING* FinalComponent,
    _In_ ULONG ProcessId,
    _In_ LARGE_INTEGER Timestamp,
    _Out_writes_(DestinationCch) PWSTR Destination,
    _In_ SIZE_T DestinationCch
    )
{
    WCHAR safeName[260];
    WCHAR stamp[64];
    SIZE_T rootLen;
    BOOLEAN rootIsNtPath;

    if (Settings == NULL || Destination == NULL || DestinationCch == 0) {
        return FALSE;
    }

    Destination[0] = L'\0';
    if (!Settings->Enabled || Settings->RootDirectory[0] == L'\0') {
        return FALSE;
    }

    rootLen = wcsnlen(Settings->RootDirectory, ARRAYSIZE(Settings->RootDirectory));
    if (rootLen == 0) {
        return FALSE;
    }

    rootIsNtPath =
        rootLen >= 4 &&
        Settings->RootDirectory[0] == L'\\' &&
        Settings->RootDirectory[1] == L'?' &&
        Settings->RootDirectory[2] == L'?' &&
        Settings->RootDirectory[3] == L'\\';

    if (!rootIsNtPath && rootLen >= 2 && Settings->RootDirectory[1] == L':') {
        if (!NT_SUCCESS(RtlStringCchCopyW(Destination, DestinationCch, L"\\??\\"))) {
            return FALSE;
        }
    }

    if (!NT_SUCCESS(RtlStringCchCatW(Destination, DestinationCch, Settings->RootDirectory))) {
        return FALSE;
    }

    rootLen = wcslen(Destination);
    if (rootLen == 0 || Destination[rootLen - 1] != L'\\') {
        if (!NT_SUCCESS(RtlStringCchCatW(Destination, DestinationCch, L"\\"))) {
            return FALSE;
        }
    }

    SanitizeComponent(FinalComponent, safeName, ARRAYSIZE(safeName));
    if (!NT_SUCCESS(RtlStringCchPrintfW(
            stamp,
            ARRAYSIZE(stamp),
            L"%08lu_%I64u_",
            ProcessId,
            Timestamp.QuadPart))) {
        return FALSE;
    }

    if (!NT_SUCCESS(RtlStringCchCatW(Destination, DestinationCch, stamp))) {
        return FALSE;
    }
    if (!NT_SUCCESS(RtlStringCchCatW(Destination, DestinationCch, safeName))) {
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
IsPathInsideQuarantineRoot(
    _In_ PCWSTR Path
    )
{
    PS_QUARANTINE_SETTINGS settings;
    SIZE_T pathLen;
    SIZE_T rootLen;
    PCWSTR root = NULL;

    if (Path == NULL) {
        return FALSE;
    }

    if (!NT_SUCCESS(PolicyEngineGetQuarantineSettings(&settings)) || !settings.Enabled) {
        return FALSE;
    }

    root = settings.RootDirectory;
    if (_wcsnicmp(root, L"\\??\\", 4) == 0) {
        root += 4;
    }

    pathLen = wcslen(Path);
    if (_wcsnicmp(Path, L"\\??\\", 4) == 0) {
        Path += 4;
        pathLen = wcslen(Path);
    }

    rootLen = wcslen(root);
    if (rootLen == 0 || pathLen < rootLen) {
        return FALSE;
    }

    if (_wcsnicmp(Path, root, rootLen) != 0) {
        return FALSE;
    }

    return pathLen == rootLen || Path[rootLen] == L'\\';
}

static
BOOLEAN
GetWriteBufferAddress(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _Outptr_result_bytebuffer_(Length) PUCHAR* Buffer,
    _In_ ULONG Length
    )
{
    PMDL mdl;

    if (Buffer == NULL) {
        return FALSE;
    }

    *Buffer = NULL;
    mdl = Data->Iopb->Parameters.Write.MdlAddress;
    if (mdl == NULL) {
        if (!NT_SUCCESS(FltLockUserBuffer(Data))) {
            return FALSE;
        }
        mdl = Data->Iopb->Parameters.Write.MdlAddress;
    }

    if (mdl != NULL) {
        *Buffer = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
        return *Buffer != NULL;
    }

    if (Data->Iopb->Parameters.Write.WriteBuffer != NULL && Length != 0) {
        *Buffer = (PUCHAR)Data->Iopb->Parameters.Write.WriteBuffer;
        return TRUE;
    }

    return FALSE;
}

static
BOOLEAN
GetReadBufferAddress(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _Outptr_result_bytebuffer_(Length) PUCHAR* Buffer,
    _In_ ULONG Length
    )
{
    PMDL mdl;

    if (Buffer == NULL) {
        return FALSE;
    }

    *Buffer = NULL;
    mdl = Data->Iopb->Parameters.Read.MdlAddress;
    if (mdl == NULL) {
        if (!NT_SUCCESS(FltLockUserBuffer(Data))) {
            return FALSE;
        }
        mdl = Data->Iopb->Parameters.Read.MdlAddress;
    }

    if (mdl != NULL) {
        *Buffer = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
        return *Buffer != NULL;
    }

    if (Data->Iopb->Parameters.Read.ReadBuffer != NULL && Length != 0) {
        *Buffer = (PUCHAR)Data->Iopb->Parameters.Read.ReadBuffer;
        return TRUE;
    }

    return FALSE;
}

static
NTSTATUS
ResolveWriteOffset(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ PPS_QUARANTINE_WRITE_CONTEXT Context,
    _In_ LARGE_INTEGER RequestedOffset,
    _Out_ PLARGE_INTEGER ResolvedOffset
    )
{
    FILE_STANDARD_INFORMATION standardInfo;
    NTSTATUS status;

    if (ResolvedOffset == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *ResolvedOffset = RequestedOffset;
    if (RequestedOffset.HighPart != -1) {
        if (Context != NULL && Context->AppendWritesToEnd) {
            FILE_STANDARD_INFORMATION appendInfo;

            status = FltQueryInformationFile(
                FltObjects->Instance,
                Context->QuarantineFileObject,
                &appendInfo,
                sizeof(appendInfo),
                FileStandardInformation,
                NULL);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            *ResolvedOffset = appendInfo.EndOfFile;
        }
        return STATUS_SUCCESS;
    }

    if (Context != NULL && Context->AppendWritesToEnd) {
        status = FltQueryInformationFile(
            FltObjects->Instance,
            Context->QuarantineFileObject,
            &standardInfo,
            sizeof(standardInfo),
            FileStandardInformation,
            NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        *ResolvedOffset = standardInfo.EndOfFile;
        return STATUS_SUCCESS;
    }

    if ((ULONG)RequestedOffset.LowPart == FILE_USE_FILE_POINTER_POSITION) {
        *ResolvedOffset = FltObjects->FileObject->CurrentByteOffset;
        return STATUS_SUCCESS;
    }

    if ((ULONG)RequestedOffset.LowPart == FILE_WRITE_TO_END_OF_FILE) {
        if (Context != NULL) {
            Context->AppendWritesToEnd = TRUE;
        }
        status = FltQueryInformationFile(
            FltObjects->Instance,
            Context->QuarantineFileObject,
            &standardInfo,
            sizeof(standardInfo),
            FileStandardInformation,
            NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        *ResolvedOffset = standardInfo.EndOfFile;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
ResolveReadOffset(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ LARGE_INTEGER RequestedOffset,
    _Out_ PLARGE_INTEGER ResolvedOffset
    )
{
    if (ResolvedOffset == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *ResolvedOffset = RequestedOffset;
    if (RequestedOffset.HighPart != -1) {
        return STATUS_SUCCESS;
    }

    if ((ULONG)RequestedOffset.LowPart == FILE_USE_FILE_POINTER_POSITION) {
        *ResolvedOffset = FltObjects->FileObject->CurrentByteOffset;
        return STATUS_SUCCESS;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
CopyOriginalFileToQuarantine(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ PPS_QUARANTINE_WRITE_CONTEXT Context
    )
{
    FILE_STANDARD_INFORMATION standardInfo;
    LARGE_INTEGER offset;
    NTSTATUS status;
    PUCHAR buffer = NULL;
    ULONG bytesRead;
    ULONG bytesWritten;

    status = FltQueryInformationFile(
        FltObjects->Instance,
        FltObjects->FileObject,
        &standardInfo,
        sizeof(standardInfo),
        FileStandardInformation,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (standardInfo.EndOfFile.QuadPart <= 0) {
        return STATUS_SUCCESS;
    }

    buffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, PS_COPY_CHUNK_SIZE, 'cqSP');
    if (buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    offset.QuadPart = 0;
    while (offset.QuadPart < standardInfo.EndOfFile.QuadPart) {
        ULONG toRead = (ULONG)min(
            (LONGLONG)PS_COPY_CHUNK_SIZE,
            standardInfo.EndOfFile.QuadPart - offset.QuadPart);

        bytesRead = 0;
        status = FltReadFile(
            FltObjects->Instance,
            FltObjects->FileObject,
            &offset,
            toRead,
            buffer,
            FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET,
            &bytesRead,
            NULL,
            NULL);
        if (!NT_SUCCESS(status) || bytesRead == 0) {
            break;
        }

        bytesWritten = 0;
        status = FltWriteFile(
            FltObjects->Instance,
            Context->QuarantineFileObject,
            &offset,
            bytesRead,
            buffer,
            FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET,
            &bytesWritten,
            NULL,
            NULL);
        if (!NT_SUCCESS(status) || bytesWritten != bytesRead) {
            break;
        }

        offset.QuadPart += bytesRead;
    }

    ExFreePoolWithTag(buffer, 'cqSP');
    return status;
}

static
NTSTATUS
OpenQuarantineTargetForWrite(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ const WCHAR* QuarantinePath,
    _Out_ PPS_QUARANTINE_WRITE_CONTEXT Context
    )
{
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatus;
    UNICODE_STRING path;
    NTSTATUS status;

    if (QuarantinePath == NULL || Context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlInitUnicodeString(&path, QuarantinePath);
    InitializeObjectAttributes(
        &objectAttributes,
        &path,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    status = FltCreateFileEx(
        gFilterHandle,
        FltObjects->Instance,
        &Context->QuarantineHandle,
        &Context->QuarantineFileObject,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
        &objectAttributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OVERWRITE_IF,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0,
        0);

    return status;
}

static
NTSTATUS
EnsureSyntheticWriteContextWithScratch(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ const PFLT_FILE_NAME_INFORMATION NameInfo,
    _Inout_ DLP_FILE_EVENT* Event,
    _Outptr_ PPS_QUARANTINE_WRITE_CONTEXT* ContextOut,
    _Inout_ PPS_SYNTHETIC_CONTEXT_SCRATCH Scratch
    )
{
    PPS_QUARANTINE_WRITE_CONTEXT context = NULL;
    PPS_QUARANTINE_WRITE_CONTEXT existing = NULL;
    NTSTATUS status;
    SIZE_T pathLen;

    if (ContextOut == NULL || NameInfo == NULL || Event == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_UNSUCCESSFUL;
    }

    UNREFERENCED_PARAMETER(Data);

    *ContextOut = NULL;
    status = FltGetStreamHandleContext(FltObjects->Instance, FltObjects->FileObject, &existing);
    if (NT_SUCCESS(status) && existing != NULL) {
        *ContextOut = existing;
        return STATUS_SUCCESS;
    }

    status = PolicyEngineGetQuarantineSettings(&Scratch->Settings);
    if (!NT_SUCCESS(status) || !Scratch->Settings.Enabled) {
        return STATUS_ACCESS_DENIED;
    }

    if (!BuildQuarantinePath(
            &Scratch->Settings,
            &NameInfo->FinalComponent,
            Event->ProcessId,
            Event->Timestamp,
            Scratch->QuarantinePath,
            ARRAYSIZE(Scratch->QuarantinePath))) {
        return STATUS_INVALID_PARAMETER;
    }

    status = FltAllocateContext(
        gFilterHandle,
        FLT_STREAMHANDLE_CONTEXT,
        sizeof(PS_QUARANTINE_WRITE_CONTEXT),
        NonPagedPoolNx,
        (PFLT_CONTEXT*)&context);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(context, sizeof(*context));
    status = OpenQuarantineTargetForWrite(FltObjects, Scratch->QuarantinePath, context);
    if (!NT_SUCCESS(status)) {
        FltReleaseContext(context);
        return status;
    }

    pathLen = wcslen(Scratch->QuarantinePath);
    context->RedirectedByCreate = FALSE;
    context->MappingRegistered = FALSE;
    context->OriginalPathLength = min(Event->FileNameLength, (USHORT)(ARRAYSIZE(context->OriginalPath) - 1));
    RtlCopyMemory(context->OriginalPath, Event->FileName, context->OriginalPathLength * sizeof(WCHAR));
    context->OriginalPath[context->OriginalPathLength] = L'\0';
    context->AppendWritesToEnd = FALSE;
    context->QuarantinePathLength = (USHORT)min(pathLen, ARRAYSIZE(context->QuarantinePath) - 1);
    RtlCopyMemory(context->QuarantinePath, Scratch->QuarantinePath, context->QuarantinePathLength * sizeof(WCHAR));
    context->QuarantinePath[context->QuarantinePathLength] = L'\0';

    status = CopyOriginalFileToQuarantine(FltObjects, context);
    if (!NT_SUCCESS(status)) {
        FltReleaseContext(context);
        return status;
    }

    status = FltSetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        FLT_SET_CONTEXT_KEEP_IF_EXISTS,
        context,
        (PFLT_CONTEXT*)&existing);
    if (status == STATUS_FLT_CONTEXT_ALREADY_DEFINED && existing != NULL) {
        FltReleaseContext(context);
        *ContextOut = existing;
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
        FltReleaseContext(context);
        return status;
    }

    context->MappingRegistered = TRUE;
    RegisterQuarantineMapping(context->OriginalPath, context->QuarantinePath);
    *ContextOut = context;
    return STATUS_SUCCESS;
}

static
NTSTATUS
EnsureSyntheticWriteContext(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ const PFLT_FILE_NAME_INFORMATION NameInfo,
    _Inout_ DLP_FILE_EVENT* Event,
    _Outptr_ PPS_QUARANTINE_WRITE_CONTEXT* ContextOut
    )
{
    PPS_SYNTHETIC_CONTEXT_SCRATCH scratch;
    NTSTATUS status;

    scratch = (PPS_SYNTHETIC_CONTEXT_SCRATCH)ExAllocatePool2(
        POOL_FLAG_PAGED,
        sizeof(*scratch),
        'wCsP');
    if (scratch == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    status = EnsureSyntheticWriteContextWithScratch(
        Data, FltObjects, NameInfo, Event, ContextOut, scratch);
    ExFreePool(scratch);
    return status;
}

static
BOOLEAN
TryRedirectToQuarantineWithScratch(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ const PFLT_FILE_NAME_INFORMATION NameInfo,
    _Inout_ DLP_FILE_EVENT* Event,
    _Outptr_opt_result_maybenull_ PPS_CREATE_REDIRECT_COMPLETION* CompletionContextOut,
    _Inout_ PPS_CREATE_REDIRECT_SCRATCH Scratch
    )
{
    NTSTATUS status;
    SIZE_T nameLen;

    if (CompletionContextOut != NULL) {
        *CompletionContextOut = NULL;
    }

    if (NameInfo == NULL || Event == NULL || !IsCreateDispositionRedirectable(Data)) {
        return FALSE;
    }

    status = PolicyEngineGetQuarantineSettings(&Scratch->Settings);
    if (!NT_SUCCESS(status) || !Scratch->Settings.Enabled) {
        return FALSE;
    }

    if (!BuildQuarantinePath(
            &Scratch->Settings,
            &NameInfo->FinalComponent,
            Event->ProcessId,
            Event->Timestamp,
            Scratch->QuarantinePath,
            ARRAYSIZE(Scratch->QuarantinePath))) {
        return FALSE;
    }

    if (Event->FileNameLength > 0) {
        USHORT copyLen = min(Event->FileNameLength, (USHORT)(ARRAYSIZE(Scratch->OriginalPath) - 1));
        RtlCopyMemory(Scratch->OriginalPath, Event->FileName, copyLen * sizeof(WCHAR));
        Scratch->OriginalPath[copyLen] = L'\0';
    } else {
        Scratch->OriginalPath[0] = L'\0';
    }

    status = IoReplaceFileObjectName(
        Data->Iopb->TargetFileObject,
        Scratch->QuarantinePath,
        (USHORT)(wcslen(Scratch->QuarantinePath) * sizeof(WCHAR)));
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    FltSetCallbackDataDirty(Data);

    nameLen = wcslen(Scratch->QuarantinePath);
    SetEventOriginalPath(Event, Scratch->OriginalPath);
    SetEventQuarantinePath(Event, Scratch->QuarantinePath);
    Event->ActionResult = ActionQuarantined;
    RegisterQuarantineMapping(Scratch->OriginalPath, Scratch->QuarantinePath);

    if (CompletionContextOut != NULL) {
        PPS_CREATE_REDIRECT_COMPLETION completion;

        completion = (PPS_CREATE_REDIRECT_COMPLETION)ExAllocatePool2(
            POOL_FLAG_PAGED,
            sizeof(*completion),
            'rcSP');
        if (completion != NULL) {
            RtlZeroMemory(completion, sizeof(*completion));
            completion->AppendWritesToEnd = ShouldInheritAppendOnlyAccess(Data) ? TRUE : FALSE;
            completion->OriginalPathLength = (USHORT)min(wcslen(Scratch->OriginalPath), ARRAYSIZE(completion->OriginalPath) - 1);
            completion->QuarantinePathLength = (USHORT)min(nameLen, ARRAYSIZE(completion->QuarantinePath) - 1);
            RtlCopyMemory(completion->OriginalPath, Scratch->OriginalPath, completion->OriginalPathLength * sizeof(WCHAR));
            completion->OriginalPath[completion->OriginalPathLength] = L'\0';
            RtlCopyMemory(completion->QuarantinePath, Scratch->QuarantinePath, completion->QuarantinePathLength * sizeof(WCHAR));
            completion->QuarantinePath[completion->QuarantinePathLength] = L'\0';
            *CompletionContextOut = completion;
        }
    }
    return TRUE;
}

static
BOOLEAN
TryRedirectToQuarantine(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ const PFLT_FILE_NAME_INFORMATION NameInfo,
    _Inout_ DLP_FILE_EVENT* Event,
    _Outptr_opt_result_maybenull_ PPS_CREATE_REDIRECT_COMPLETION* CompletionContextOut
    )
{
    PPS_CREATE_REDIRECT_SCRATCH scratch;
    BOOLEAN redirected;

    scratch = (PPS_CREATE_REDIRECT_SCRATCH)ExAllocatePool2(
        POOL_FLAG_PAGED,
        sizeof(*scratch),
        'rCsP');
    if (scratch == NULL) return FALSE;
    redirected = TryRedirectToQuarantineWithScratch(
        Data, NameInfo, Event, CompletionContextOut, scratch);
    ExFreePool(scratch);
    return redirected;
}

static
FLT_PREOP_CALLBACK_STATUS
PerformSyntheticQuarantineRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ PPS_QUARANTINE_WRITE_CONTEXT Context,
    _Inout_ DLP_FILE_EVENT* Event
    )
{
    LARGE_INTEGER resolvedOffset;
    ULONG length;
    PUCHAR buffer = NULL;
    ULONG bytesRead = 0;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        Data->IoStatus.Status = STATUS_UNSUCCESSFUL;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    if (Context == NULL || Context->QuarantineFileObject == NULL) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    length = Data->Iopb->Parameters.Read.Length;
    if (length == 0) {
        Event->ActionResult = ActionQuarantined;
        SetEventOriginalPath(Event, Context->OriginalPath);
        SetEventQuarantinePath(Event, Context->QuarantinePath);
        Data->IoStatus.Status = STATUS_SUCCESS;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    if (!GetReadBufferAddress(Data, &buffer, length)) {
        Data->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    status = ResolveReadOffset(
        FltObjects,
        Data->Iopb->Parameters.Read.ByteOffset,
        &resolvedOffset);
    if (!NT_SUCCESS(status)) {
        Data->IoStatus.Status = status;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    status = FltReadFile(
        FltObjects->Instance,
        Context->QuarantineFileObject,
        &resolvedOffset,
        length,
        buffer,
        FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET,
        &bytesRead,
        NULL,
        NULL);
    if (!NT_SUCCESS(status)) {
        Data->IoStatus.Status = status;
        Data->IoStatus.Information = bytesRead;
        return FLT_PREOP_COMPLETE;
    }

    if (Data->Iopb->Parameters.Read.ByteOffset.HighPart == -1 &&
        (ULONG)Data->Iopb->Parameters.Read.ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION) {
        FltObjects->FileObject->CurrentByteOffset.QuadPart = resolvedOffset.QuadPart + bytesRead;
    }

    Event->ActionResult = ActionQuarantined;
    SetEventOriginalPath(Event, Context->OriginalPath);
    SetEventQuarantinePath(Event, Context->QuarantinePath);
    Data->IoStatus.Status = STATUS_SUCCESS;
    Data->IoStatus.Information = bytesRead;
    return FLT_PREOP_COMPLETE;
}

static
NTSTATUS
CompleteNameInformationFromContext(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PPS_QUARANTINE_WRITE_CONTEXT Context
    )
{
    PVOID buffer;
    ULONG length;
    PFILE_NAME_INFORMATION nameInfo;
    ULONG required;

    if (Context == NULL || Context->OriginalPathLength == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    buffer = Data->Iopb->Parameters.QueryFileInformation.InfoBuffer;
    length = Data->Iopb->Parameters.QueryFileInformation.Length;
    if (buffer == NULL || length < sizeof(FILE_NAME_INFORMATION)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    nameInfo = (PFILE_NAME_INFORMATION)buffer;
    required = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) +
        Context->OriginalPathLength * sizeof(WCHAR);
    if (length < required) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    __try {
        nameInfo->FileNameLength = Context->OriginalPathLength * sizeof(WCHAR);
        RtlCopyMemory(nameInfo->FileName, Context->OriginalPath, nameInfo->FileNameLength);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    Data->IoStatus.Information = required;
    return STATUS_SUCCESS;
}

static
NTSTATUS
CompleteQueryInformationFromContext(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ PPS_QUARANTINE_WRITE_CONTEXT Context
    )
{
    FILE_INFORMATION_CLASS infoClass;
    PVOID buffer;
    ULONG length;
    ULONG bytesReturned = 0;
    NTSTATUS status;

    if (Context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    infoClass = Data->Iopb->Parameters.QueryFileInformation.FileInformationClass;
    if (infoClass == FileNameInformation || infoClass == FileNormalizedNameInformation) {
        return CompleteNameInformationFromContext(Data, Context);
    }

    if (Context->QuarantineFileObject == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    buffer = Data->Iopb->Parameters.QueryFileInformation.InfoBuffer;
    length = Data->Iopb->Parameters.QueryFileInformation.Length;
    if (buffer == NULL || length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    status = FltQueryInformationFile(
        FltObjects->Instance,
        Context->QuarantineFileObject,
        buffer,
        length,
        infoClass,
        &bytesReturned);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    Data->IoStatus.Information = bytesReturned;
    return STATUS_SUCCESS;
}

static
BOOLEAN
BuildDirectoryEntryFullPath(
    _In_ PCWSTR DirectoryPath,
    _In_ const WCHAR* FileName,
    _In_ USHORT FileNameLengthChars,
    _Out_writes_(OutputCch) PWCHAR Output,
    _In_ SIZE_T OutputCch
    )
{
    size_t directoryLen;

    if (DirectoryPath == NULL || FileName == NULL || Output == NULL || OutputCch == 0) {
        return FALSE;
    }

    if (!NT_SUCCESS(RtlStringCchCopyW(Output, OutputCch, DirectoryPath))) {
        return FALSE;
    }

    directoryLen = wcslen(Output);
    if (directoryLen == 0 || Output[directoryLen - 1] != L'\\') {
        if (!NT_SUCCESS(RtlStringCchCatW(Output, OutputCch, L"\\"))) {
            return FALSE;
        }
    }

    if (!NT_SUCCESS(RtlStringCchCatNW(Output, OutputCch, FileName, FileNameLengthChars))) {
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
QueryQuarantineFileNetworkInfo(
    _In_ PCWSTR QuarantinePath,
    _Out_ PFILE_NETWORK_OPEN_INFORMATION NetworkInfo
    )
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatus;
    HANDLE handle = NULL;
    NTSTATUS status;

    if (QuarantinePath == NULL || NetworkInfo == NULL) {
        return FALSE;
    }

    RtlInitUnicodeString(&path, QuarantinePath);
    InitializeObjectAttributes(&objectAttributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwCreateFile(
        &handle,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &objectAttributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    status = ZwQueryInformationFile(
        handle,
        &ioStatus,
        NetworkInfo,
        sizeof(*NetworkInfo),
        FileNetworkOpenInformation);
    ZwClose(handle);
    return NT_SUCCESS(status);
}

static
VOID
RewriteDirectoryEntryMetadata(
    _In_ FILE_INFORMATION_CLASS InfoClass,
    _Inout_ PVOID Entry,
    _In_ const FILE_NETWORK_OPEN_INFORMATION* NetworkInfo
    )
{
    if (Entry == NULL || NetworkInfo == NULL) {
        return;
    }

    switch (InfoClass) {
        case FileDirectoryInformation:
        {
            PFILE_DIRECTORY_INFORMATION info = (PFILE_DIRECTORY_INFORMATION)Entry;
            info->CreationTime = NetworkInfo->CreationTime;
            info->LastAccessTime = NetworkInfo->LastAccessTime;
            info->LastWriteTime = NetworkInfo->LastWriteTime;
            info->ChangeTime = NetworkInfo->ChangeTime;
            info->EndOfFile = NetworkInfo->EndOfFile;
            info->AllocationSize = NetworkInfo->AllocationSize;
            info->FileAttributes = NetworkInfo->FileAttributes;
            break;
        }
        case FileFullDirectoryInformation:
        {
            PFILE_FULL_DIR_INFORMATION info = (PFILE_FULL_DIR_INFORMATION)Entry;
            info->CreationTime = NetworkInfo->CreationTime;
            info->LastAccessTime = NetworkInfo->LastAccessTime;
            info->LastWriteTime = NetworkInfo->LastWriteTime;
            info->ChangeTime = NetworkInfo->ChangeTime;
            info->EndOfFile = NetworkInfo->EndOfFile;
            info->AllocationSize = NetworkInfo->AllocationSize;
            info->FileAttributes = NetworkInfo->FileAttributes;
            break;
        }
        case FileBothDirectoryInformation:
        {
            PFILE_BOTH_DIR_INFORMATION info = (PFILE_BOTH_DIR_INFORMATION)Entry;
            info->CreationTime = NetworkInfo->CreationTime;
            info->LastAccessTime = NetworkInfo->LastAccessTime;
            info->LastWriteTime = NetworkInfo->LastWriteTime;
            info->ChangeTime = NetworkInfo->ChangeTime;
            info->EndOfFile = NetworkInfo->EndOfFile;
            info->AllocationSize = NetworkInfo->AllocationSize;
            info->FileAttributes = NetworkInfo->FileAttributes;
            break;
        }
        case FileIdBothDirectoryInformation:
        {
            PFILE_ID_BOTH_DIR_INFORMATION info = (PFILE_ID_BOTH_DIR_INFORMATION)Entry;
            info->CreationTime = NetworkInfo->CreationTime;
            info->LastAccessTime = NetworkInfo->LastAccessTime;
            info->LastWriteTime = NetworkInfo->LastWriteTime;
            info->ChangeTime = NetworkInfo->ChangeTime;
            info->EndOfFile = NetworkInfo->EndOfFile;
            info->AllocationSize = NetworkInfo->AllocationSize;
            info->FileAttributes = NetworkInfo->FileAttributes;
            break;
        }
        case FileIdFullDirectoryInformation:
        {
            PFILE_ID_FULL_DIR_INFORMATION info = (PFILE_ID_FULL_DIR_INFORMATION)Entry;
            info->CreationTime = NetworkInfo->CreationTime;
            info->LastAccessTime = NetworkInfo->LastAccessTime;
            info->LastWriteTime = NetworkInfo->LastWriteTime;
            info->ChangeTime = NetworkInfo->ChangeTime;
            info->EndOfFile = NetworkInfo->EndOfFile;
            info->AllocationSize = NetworkInfo->AllocationSize;
            info->FileAttributes = NetworkInfo->FileAttributes;
            break;
        }
        default:
            break;
    }
}

static
BOOLEAN
GetDirectoryEntryView(
    _In_ FILE_INFORMATION_CLASS InfoClass,
    _Inout_ PVOID Entry,
    _Out_ PULONG NextEntryOffset,
    _Out_ PULONG FileNameLength,
    _Outptr_ PWCHAR* FileName
    )
{
    if (Entry == NULL || NextEntryOffset == NULL || FileNameLength == NULL || FileName == NULL) {
        return FALSE;
    }

    switch (InfoClass) {
        case FileDirectoryInformation:
        {
            PFILE_DIRECTORY_INFORMATION info = (PFILE_DIRECTORY_INFORMATION)Entry;
            *NextEntryOffset = info->NextEntryOffset;
            *FileNameLength = info->FileNameLength;
            *FileName = info->FileName;
            return TRUE;
        }
        case FileFullDirectoryInformation:
        {
            PFILE_FULL_DIR_INFORMATION info = (PFILE_FULL_DIR_INFORMATION)Entry;
            *NextEntryOffset = info->NextEntryOffset;
            *FileNameLength = info->FileNameLength;
            *FileName = info->FileName;
            return TRUE;
        }
        case FileBothDirectoryInformation:
        {
            PFILE_BOTH_DIR_INFORMATION info = (PFILE_BOTH_DIR_INFORMATION)Entry;
            *NextEntryOffset = info->NextEntryOffset;
            *FileNameLength = info->FileNameLength;
            *FileName = info->FileName;
            return TRUE;
        }
        case FileIdBothDirectoryInformation:
        {
            PFILE_ID_BOTH_DIR_INFORMATION info = (PFILE_ID_BOTH_DIR_INFORMATION)Entry;
            *NextEntryOffset = info->NextEntryOffset;
            *FileNameLength = info->FileNameLength;
            *FileName = info->FileName;
            return TRUE;
        }
        case FileIdFullDirectoryInformation:
        {
            PFILE_ID_FULL_DIR_INFORMATION info = (PFILE_ID_FULL_DIR_INFORMATION)Entry;
            *NextEntryOffset = info->NextEntryOffset;
            *FileNameLength = info->FileNameLength;
            *FileName = info->FileName;
            return TRUE;
        }
        default:
            return FALSE;
    }
}

static
FLT_PREOP_CALLBACK_STATUS
PerformSyntheticQuarantineWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Inout_ PPS_QUARANTINE_WRITE_CONTEXT Context,
    _Inout_ DLP_FILE_EVENT* Event
    )
{
    LARGE_INTEGER resolvedOffset;
    ULONG length;
    PUCHAR buffer = NULL;
    ULONG bytesWritten = 0;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        Data->IoStatus.Status = STATUS_UNSUCCESSFUL;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    length = Data->Iopb->Parameters.Write.Length;
    if (length == 0) {
        Event->ActionResult = ActionQuarantined;
        SetEventOriginalPath(Event, Context->OriginalPath);
        SetEventQuarantinePath(Event, Context->QuarantinePath);
        Data->IoStatus.Status = STATUS_SUCCESS;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    if (!GetWriteBufferAddress(Data, &buffer, length)) {
        Data->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    status = ResolveWriteOffset(
        FltObjects,
        Context,
        Data->Iopb->Parameters.Write.ByteOffset,
        &resolvedOffset);
    if (!NT_SUCCESS(status)) {
        Data->IoStatus.Status = status;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    status = FltWriteFile(
        FltObjects->Instance,
        Context->QuarantineFileObject,
        &resolvedOffset,
        length,
        buffer,
        FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET,
        &bytesWritten,
        NULL,
        NULL);
    if (!NT_SUCCESS(status) || bytesWritten != length) {
        Data->IoStatus.Status = !NT_SUCCESS(status) ? status : STATUS_UNSUCCESSFUL;
        Data->IoStatus.Information = bytesWritten;
        return FLT_PREOP_COMPLETE;
    }

    if (Data->Iopb->Parameters.Write.ByteOffset.HighPart == -1 &&
        (ULONG)Data->Iopb->Parameters.Write.ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION) {
        FltObjects->FileObject->CurrentByteOffset.QuadPart = resolvedOffset.QuadPart + bytesWritten;
    }

    Event->ActionResult = ActionQuarantined;
    SetEventOriginalPath(Event, Context->OriginalPath);
    SetEventQuarantinePath(Event, Context->QuarantinePath);
    Data->IoStatus.Status = STATUS_SUCCESS;
    Data->IoStatus.Information = bytesWritten;
    return FLT_PREOP_COMPLETE;
}

static
FLT_PREOP_CALLBACK_STATUS
HandleClassifiedFileOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ DLP_EVENT_TYPE EventType,
    _In_ BOOLEAN AllowQuarantineRedirect,
    _Inout_opt_ PVOID* CompletionContext
    )
{
    PDLP_FILE_EVENT event;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PPS_QUARANTINE_WRITE_CONTEXT writeContext = NULL;
    PPS_CREATE_REDIRECT_COMPLETION redirectCompletion = NULL;
    ACTION_RESULT action;
    NTSTATUS status;
    FLT_PREOP_CALLBACK_STATUS preopStatus;

    event = FileEventAllocate();
    if (event == NULL) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!BuildFileEvent(Data, EventType, event, &nameInfo)) {
        FileEventFree(event);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (ProtectIsPathProtected(event->FileName) &&
        (EventType != EventFileCreate ||
         (Data->Iopb->Parameters.Create.SecurityContext != NULL &&
          FlagOn(Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess,
              DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
              WRITE_DAC | WRITE_OWNER)))) {
        event->ActionResult = ActionBlocked;
        FileEventEnqueue(event);
        FltReleaseFileNameInformation(nameInfo);
        FileEventFree(event);
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    if (EventType == EventFileWrite) {
        status = FltGetStreamHandleContext(FltObjects->Instance, FltObjects->FileObject, &writeContext);
        if (NT_SUCCESS(status) && writeContext != NULL) {
            preopStatus = PerformSyntheticQuarantineWrite(Data, FltObjects, writeContext, event);
            FileEventEnqueue(event);
            FltReleaseContext(writeContext);
            FltReleaseFileNameInformation(nameInfo);
            FileEventFree(event);
            return preopStatus;
        }
        if (IsPathInsideQuarantineRoot(event->FileName)) {
            event->ActionResult = ActionQuarantined;
            SetEventOriginalPath(event, NULL);
            SetEventQuarantinePath(event, event->FileName);
            FileEventEnqueue(event);
            FltReleaseFileNameInformation(nameInfo);
            FileEventFree(event);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
    }

    action = ProtectIsMaintenanceProcess(event->ProcessId)
        ? ActionAllowed
        : PolicyEngineQueryFileAction(event->ProcessId, event->FileName, EventType);
    event->ActionResult = action;

    if (action == ActionBlocked &&
        AllowQuarantineRedirect &&
        TryRedirectToQuarantine(Data, nameInfo, event, &redirectCompletion)) {
        if (CompletionContext != NULL) {
            *CompletionContext = redirectCompletion;
        }
        FileEventEnqueue(event);
        FltReleaseFileNameInformation(nameInfo);
        FileEventFree(event);
        return redirectCompletion != NULL ? FLT_PREOP_SUCCESS_WITH_CALLBACK : FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (action == ActionBlocked && EventType == EventFileWrite) {
        status = EnsureSyntheticWriteContext(Data, FltObjects, nameInfo, event, &writeContext);
        if (NT_SUCCESS(status) && writeContext != NULL) {
            preopStatus = PerformSyntheticQuarantineWrite(Data, FltObjects, writeContext, event);
            FileEventEnqueue(event);
            FltReleaseContext(writeContext);
            FltReleaseFileNameInformation(nameInfo);
            FileEventFree(event);
            return preopStatus;
        }
    }

    FileEventEnqueue(event);
    FltReleaseFileNameInformation(nameInfo);
    FileEventFree(event);

    if (action == ActionBlocked) {
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
PostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    PPS_CREATE_REDIRECT_COMPLETION redirectCompletion = (PPS_CREATE_REDIRECT_COMPLETION)CompletionContext;
    PPS_QUARANTINE_WRITE_CONTEXT context = NULL;
    PPS_QUARANTINE_WRITE_CONTEXT existing = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Flags);

    if (redirectCompletion == NULL) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (NT_SUCCESS(Data->IoStatus.Status) &&
        FltObjects != NULL &&
        FltObjects->Instance != NULL &&
        FltObjects->FileObject != NULL &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        status = FltAllocateContext(
            gFilterHandle,
            FLT_STREAMHANDLE_CONTEXT,
            sizeof(PS_QUARANTINE_WRITE_CONTEXT),
            NonPagedPoolNx,
            (PFLT_CONTEXT*)&context);
        if (NT_SUCCESS(status) && context != NULL) {
            RtlZeroMemory(context, sizeof(*context));
            context->RedirectedByCreate = TRUE;
            context->MappingRegistered = TRUE;
            context->AppendWritesToEnd = redirectCompletion->AppendWritesToEnd;
            context->OriginalPathLength = min(
                redirectCompletion->OriginalPathLength,
                (USHORT)(ARRAYSIZE(context->OriginalPath) - 1));
            context->QuarantinePathLength = min(
                redirectCompletion->QuarantinePathLength,
                (USHORT)(ARRAYSIZE(context->QuarantinePath) - 1));
            RtlCopyMemory(
                context->OriginalPath,
                redirectCompletion->OriginalPath,
                context->OriginalPathLength * sizeof(WCHAR));
            context->OriginalPath[context->OriginalPathLength] = L'\0';
            RtlCopyMemory(
                context->QuarantinePath,
                redirectCompletion->QuarantinePath,
                context->QuarantinePathLength * sizeof(WCHAR));
            context->QuarantinePath[context->QuarantinePathLength] = L'\0';

            context->QuarantineFileObject = FltObjects->FileObject;
            ObReferenceObject(context->QuarantineFileObject);
            RegisterQuarantineMapping(context->OriginalPath, context->QuarantinePath);

            status = FltSetStreamHandleContext(
                FltObjects->Instance,
                FltObjects->FileObject,
                FLT_SET_CONTEXT_KEEP_IF_EXISTS,
                context,
                (PFLT_CONTEXT*)&existing);
            if (existing != NULL) {
                FltReleaseContext(existing);
            }
            FltReleaseContext(context);
        }
    }

    ExFreePool(redirectCompletion);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

FLT_PREOP_CALLBACK_STATUS
PreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    PDLP_FILE_EVENT event;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PPS_QUARANTINE_WRITE_CONTEXT writeContext = NULL;
    NTSTATUS status;
    FLT_PREOP_CALLBACK_STATUS preopStatus;

    UNREFERENCED_PARAMETER(CompletionContext);
    PAGED_CODE();

    if (Data->RequestorMode != UserMode ||
        FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    if (ShouldBypassFileOperation(Data)) return FLT_PREOP_SUCCESS_NO_CALLBACK;

    event = FileEventAllocate();
    if (event == NULL) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!BuildFileEvent(Data, EventFileRead, event, &nameInfo)) {
        FileEventFree(event);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    status = FltGetStreamHandleContext(FltObjects->Instance, FltObjects->FileObject, &writeContext);
    if (NT_SUCCESS(status) && writeContext != NULL) {
        preopStatus = PerformSyntheticQuarantineRead(Data, FltObjects, writeContext, event);
        FileEventEnqueue(event);
        FltReleaseContext(writeContext);
        FltReleaseFileNameInformation(nameInfo);
        FileEventFree(event);
        return preopStatus;
    }

    FileEventEnqueue(event);
    FltReleaseFileNameInformation(nameInfo);
    FileEventFree(event);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_PREOP_CALLBACK_STATUS
PreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    UNREFERENCED_PARAMETER(CompletionContext);
    PAGED_CODE();

    if (Data->RequestorMode != UserMode ||
        FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    if (ShouldBypassFileOperation(Data)) return FLT_PREOP_SUCCESS_NO_CALLBACK;

    return HandleClassifiedFileOperation(Data, FltObjects, EventFileCreate, TRUE, CompletionContext);
}

FLT_PREOP_CALLBACK_STATUS
PreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    PPS_QUARANTINE_WRITE_CONTEXT writeContext = NULL;
    NTSTATUS status;
    PDLP_FILE_EVENT event;
    FLT_PREOP_CALLBACK_STATUS preopStatus;

    UNREFERENCED_PARAMETER(CompletionContext);
    PAGED_CODE();

    if (FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO)) {
        PFLT_FILE_NAME_INFORMATION protectedName = NULL;

        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }

        status = FltGetFileNameInformationUnsafe(
            FltObjects->FileObject,
            FltObjects->Instance,
            FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
            &protectedName);
        if (NT_SUCCESS(status) && protectedName != NULL) {
            if (ProtectIsPathProtected(protectedName->Name.Buffer)) {
                FltReleaseFileNameInformation(protectedName);
                Data->IoStatus.Status = STATUS_ACCESS_DENIED;
                Data->IoStatus.Information = 0;
                return FLT_PREOP_COMPLETE;
            }
            FltReleaseFileNameInformation(protectedName);
        }

        status = FltGetStreamHandleContext(FltObjects->Instance, FltObjects->FileObject, &writeContext);
        if (!NT_SUCCESS(status) || writeContext == NULL) {
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }

        if (writeContext->RedirectedByCreate) {
            FltReleaseContext(writeContext);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }

        event = FileEventAllocate();
        if (event == NULL) {
            FltReleaseContext(writeContext);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
        InitializeContextOnlyFileEvent(event, writeContext);
        event->FileSize = Data->Iopb->Parameters.Write.Length;
        preopStatus = PerformSyntheticQuarantineWrite(Data, FltObjects, writeContext, event);
        FileEventEnqueue(event);
        FileEventFree(event);
        FltReleaseContext(writeContext);
        return preopStatus;
    }

    if (Data->RequestorMode != UserMode) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    if (ShouldBypassFileOperation(Data)) return FLT_PREOP_SUCCESS_NO_CALLBACK;

    return HandleClassifiedFileOperation(Data, FltObjects, EventFileWrite, FALSE, NULL);
}

FLT_PREOP_CALLBACK_STATUS
PreQueryInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    PPS_QUARANTINE_WRITE_CONTEXT writeContext = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(CompletionContext);
    PAGED_CODE();

    if (Data->RequestorMode != UserMode) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    status = FltGetStreamHandleContext(FltObjects->Instance, FltObjects->FileObject, &writeContext);
    if (!NT_SUCCESS(status) || writeContext == NULL) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    status = CompleteQueryInformationFromContext(Data, FltObjects, writeContext);
    FltReleaseContext(writeContext);
    if (status == STATUS_NOT_SUPPORTED) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    Data->IoStatus.Status = status;
    if (!NT_SUCCESS(status)) {
        Data->IoStatus.Information = 0;
    }
    return FLT_PREOP_COMPLETE;
}

FLT_PREOP_CALLBACK_STATUS
PreDirectoryControl(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    PAGED_CODE();
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static
FLT_POSTOP_CALLBACK_STATUS
PostDirectoryControlWithScratch(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags,
    _Inout_ PPS_DIRECTORY_POST_SCRATCH Scratch
    )
{
    PFLT_FILE_NAME_INFORMATION directoryName = NULL;
    PVOID entry;
    FILE_INFORMATION_CLASS infoClass;
    ULONG nextOffset;
    ULONG fileNameLength;
    PWCHAR fileName;

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);
    PAGED_CODE();

    if (!NT_SUCCESS(Data->IoStatus.Status) ||
        Data->Iopb->MinorFunction != IRP_MN_QUERY_DIRECTORY ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    infoClass = Data->Iopb->Parameters.DirectoryControl.QueryDirectory.FileInformationClass;
    if (infoClass != FileDirectoryInformation &&
        infoClass != FileFullDirectoryInformation &&
        infoClass != FileBothDirectoryInformation &&
        infoClass != FileIdBothDirectoryInformation &&
        infoClass != FileIdFullDirectoryInformation) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (!NT_SUCCESS(FltGetFileNameInformation(
            Data,
            FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
            &directoryName)) ||
        directoryName == NULL) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    FltParseFileNameInformation(directoryName);
    entry = Data->Iopb->Parameters.DirectoryControl.QueryDirectory.DirectoryBuffer;
    if (entry == NULL) {
        FltReleaseFileNameInformation(directoryName);
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    while (entry != NULL &&
           GetDirectoryEntryView(infoClass, entry, &nextOffset, &fileNameLength, &fileName)) {
        if (BuildDirectoryEntryFullPath(
                directoryName->Name.Buffer,
                fileName,
                (USHORT)(fileNameLength / sizeof(WCHAR)),
                Scratch->OriginalPath,
                ARRAYSIZE(Scratch->OriginalPath)) &&
            LookupQuarantineMapping(
                Scratch->OriginalPath,
                Scratch->QuarantinePath,
                ARRAYSIZE(Scratch->QuarantinePath)) &&
            QueryQuarantineFileNetworkInfo(Scratch->QuarantinePath, &Scratch->NetworkInfo)) {
            RewriteDirectoryEntryMetadata(infoClass, entry, &Scratch->NetworkInfo);
        }

        if (nextOffset == 0) {
            break;
        }
        entry = (PUCHAR)entry + nextOffset;
    }

    FltReleaseFileNameInformation(directoryName);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

FLT_POSTOP_CALLBACK_STATUS
PostDirectoryControl(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    PPS_DIRECTORY_POST_SCRATCH scratch;
    FLT_POSTOP_CALLBACK_STATUS status;

    PAGED_CODE();
    scratch = (PPS_DIRECTORY_POST_SCRATCH)ExAllocatePool2(
        POOL_FLAG_PAGED,
        sizeof(*scratch),
        'dPsP');
    if (scratch == NULL) return FLT_POSTOP_FINISHED_PROCESSING;
    status = PostDirectoryControlWithScratch(
        Data, FltObjects, CompletionContext, Flags, scratch);
    ExFreePool(scratch);
    return status;
}

FLT_PREOP_CALLBACK_STATUS
PreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    FILE_INFORMATION_CLASS infoClass;
    DLP_EVENT_TYPE eventType;

    UNREFERENCED_PARAMETER(CompletionContext);
    PAGED_CODE();

    if (Data->RequestorMode != UserMode) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    if (ShouldBypassFileOperation(Data)) return FLT_PREOP_SUCCESS_NO_CALLBACK;

    infoClass = Data->Iopb->Parameters.SetFileInformation.FileInformationClass;
    if (infoClass == FileDispositionInformation ||
        infoClass == FileDispositionInformationEx) {
        eventType = EventFileDelete;
    } else if (infoClass == FileRenameInformation ||
               infoClass == FileRenameInformationEx) {
        eventType = EventFileRename;
    } else {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    return HandleClassifiedFileOperation(Data, FltObjects, eventType, FALSE, NULL);
}
