/*
 * classification.c - stream payload inspection helpers and callouts
 */

#include "classification.h"
#include "sni_capture.h"
#include "net_event.h"
#include "../filter/process_tracker.h"
#include "../policy/policy_engine.h"
#include "../core/protect.h"
#include "../common/shared_events.h"
#include <ntstrsafe.h>

#define PS_STREAM_INSPECT_BYTES 2048
#define PS_STREAM_FLOW_CACHE_SIZE 256
#define PS_STREAM_DUP_WINDOW_100NS 20000000LL
#define PS_FTP_DATA_EXPECTATION_SIZE 128
#define PS_FTP_ACTIVE_FLOW_SIZE 128
#define PS_FTP_DIRECTORY_LINE_BYTES 768
#define PS_FTP_DIRECTORY_NAME_CHARS 256
#define PS_FTP_DIRECTORY_EVENTS_PER_CALLBACK 32
#define PS_FTP_CONTENT_TAIL_BYTES (PS_FTP_CONTENT_LEN - 1)
#define PS_FTP_CONTENT_PREVIEW_BYTES 640
#define PS_FTP_CONTENT_CHUNK_BYTES 512
#define PS_HTTP_REASSEMBLY_BYTES 8192
#define PS_HTTP_BODY_PREVIEW_BYTES 2048
#define PS_HTTP_CONTENT_TYPE_CHARS 64
#define PS_JSON_PATH_CHARS 128
#define PS_JSON_SEMANTIC_MAX_DEPTH 8

typedef enum _STREAM_SIGNAL_KIND {
    StreamSignalHttpRequest = 0,
    StreamSignalHttpResponse,
    StreamSignalSni,
    StreamSignalFtpLine
} STREAM_SIGNAL_KIND;

typedef struct _STREAM_FLOW_CACHE_ENTRY {
    UINT64 FlowHandle;
    LARGE_INTEGER LastSeen;
    UINT32 LastHttpRequestHash;
    LARGE_INTEGER LastHttpRequestTime;
    UINT32 LastHttpResponseHash;
    LARGE_INTEGER LastHttpResponseTime;
    UINT32 LastSniHash;
    LARGE_INTEGER LastSniTime;
    UINT32 LastFtpLineHash;
    LARGE_INTEGER LastFtpLineTime;
    WCHAR LastFtpCommand[PS_URL_LEN];
    WCHAR PendingTransferCommand[PS_URL_LEN];
    BOOLEAN FtpAuthenticated;
    BOOLEAN TransferInProgress;
    BOOLEAN TransferDataSeen;
    BOOLEAN ControlInfoValid;
    BOOLEAN IsV6;
    BOOLEAN InUse;
    ULONG ProcessId;
    UINT16 ControlLocalPort;
    UINT16 ControlRemotePort;
    UCHAR ControlLocalAddress[16];
    UCHAR ControlRemoteAddress[16];
    UCHAR HttpReqBuffer[PS_HTTP_REASSEMBLY_BYTES];
    ULONG HttpReqBufferedBytes;
    ULONG HttpReqBodyRemaining;
    BOOLEAN HttpReqChunked;
    ULONG HttpReqChunkRemaining;
    BOOLEAN HttpReqChunkNeedCrlf;
    BOOLEAN HttpReqChunkTrailer;
    BOOLEAN HttpReqCaptureBody;
    WCHAR HttpReqContentType[PS_HTTP_CONTENT_TYPE_CHARS];
    WCHAR HttpReqContentEncoding[32];
    UCHAR HttpReqBodyPreview[PS_HTTP_BODY_PREVIEW_BYTES];
    ULONG HttpReqBodyPreviewBytes;
    UCHAR HttpRespBuffer[PS_HTTP_REASSEMBLY_BYTES];
    ULONG HttpRespBufferedBytes;
    ULONG HttpRespBodyRemaining;
    BOOLEAN HttpRespChunked;
    ULONG HttpRespChunkRemaining;
    BOOLEAN HttpRespChunkNeedCrlf;
    BOOLEAN HttpRespChunkTrailer;
    BOOLEAN HttpRespCaptureBody;
    WCHAR HttpRespContentType[PS_HTTP_CONTENT_TYPE_CHARS];
    WCHAR HttpRespContentEncoding[32];
    UCHAR HttpRespBodyPreview[PS_HTTP_BODY_PREVIEW_BYTES];
    ULONG HttpRespBodyPreviewBytes;
} STREAM_FLOW_CACHE_ENTRY, *PSTREAM_FLOW_CACHE_ENTRY;

typedef struct _FTP_FLOW_CONTROL_INFO {
    BOOLEAN IsV6;
    UCHAR LocalAddress[16];
    UCHAR RemoteAddress[16];
} FTP_FLOW_CONTROL_INFO, *PFTP_FLOW_CONTROL_INFO;

typedef struct _FTP_DATA_EXPECTATION {
    BOOLEAN InUse;
    BOOLEAN IsV6;
    BOOLEAN OutboundExpected;
    ULONG ProcessId;
    UINT16 ExpectedLocalPort;
    UINT16 ExpectedRemotePort;
    BOOLEAN HasLocalAddress;
    BOOLEAN HasRemoteAddress;
    UCHAR LocalAddress[16];
    UCHAR RemoteAddress[16];
    WCHAR Command[PS_URL_LEN];
    LARGE_INTEGER Created;
} FTP_DATA_EXPECTATION, *PFTP_DATA_EXPECTATION;

typedef struct _FTP_ACTIVE_FLOW {
    BOOLEAN InUse;
    BOOLEAN IsV6;
    BOOLEAN OutboundObserved;
    ULONG ProcessId;
    UINT16 LocalPort;
    UINT16 RemotePort;
    UCHAR LocalAddress[16];
    UCHAR RemoteAddress[16];
    WCHAR Command[PS_URL_LEN];
    ULONGLONG TotalBytesObserved;
    ULONGLONG TotalBytesMissed;
    ULONGLONG ListedFileBytes;
    ULONG DirectoryEntryCount;
    ULONG DirectoryFileCount;
    ULONG DirectoryDirectoryCount;
    ULONG DirectoryLinkCount;
    ULONG DirectoryEntryEventsSuppressed;
    UCHAR DirectoryLineBuffer[PS_FTP_DIRECTORY_LINE_BYTES];
    USHORT DirectoryLineBytes;
    BOOLEAN DirectoryLineOverflow;
    BOOLEAN DirectoryHadTruncatedLine;
    UCHAR ContentTail[PS_FTP_CONTENT_TAIL_BYTES];
    USHORT ContentTailBytes;
    WCHAR DetectedContentType[32];
    UINT32 LastPayloadHash;
    LARGE_INTEGER LastPayloadTime;
    LARGE_INTEGER Created;
    LARGE_INTEGER LastSeen;
} FTP_ACTIVE_FLOW, *PFTP_ACTIVE_FLOW;

typedef enum _FTP_DIRECTORY_ENTRY_TYPE {
    FtpDirectoryEntryUnknown = 0,
    FtpDirectoryEntryFile,
    FtpDirectoryEntryDirectory,
    FtpDirectoryEntryLink
} FTP_DIRECTORY_ENTRY_TYPE;

typedef struct _FTP_DIRECTORY_ENTRY {
    FTP_DIRECTORY_ENTRY_TYPE Type;
    BOOLEAN HasSize;
    BOOLEAN HasAllocationBlocks;
    ULONGLONG Size;
    ULONGLONG AllocationBlocks;
    WCHAR Format[12];
    WCHAR Permissions[16];
    WCHAR Owner[64];
    WCHAR Group[64];
    WCHAR Modified[40];
    WCHAR Name[PS_FTP_DIRECTORY_NAME_CHARS];
    WCHAR LinkTarget[PS_FTP_DIRECTORY_NAME_CHARS];
} FTP_DIRECTORY_ENTRY, *PFTP_DIRECTORY_ENTRY;

typedef struct _FTP_DATA_PREVIEW_SCRATCH {
    WCHAR Command[PS_URL_LEN];
    WCHAR Decorated[1024];
    WCHAR ContentType[32];
    UCHAR DirectoryLine[PS_FTP_DIRECTORY_LINE_BYTES];
    FTP_DIRECTORY_ENTRY DirectoryEntry;
} FTP_DATA_PREVIEW_SCRATCH, *PFTP_DATA_PREVIEW_SCRATCH;

typedef struct _FTP_DIRECTORY_PARSE_SCRATCH {
    WCHAR Text[PS_FTP_DIRECTORY_LINE_BYTES];
    WCHAR ParseBuffer[PS_FTP_DIRECTORY_LINE_BYTES];
} FTP_DIRECTORY_PARSE_SCRATCH, *PFTP_DIRECTORY_PARSE_SCRATCH;

typedef struct _FTP_DIRECTORY_EVENT_SCRATCH {
    FTP_DIRECTORY_ENTRY Entry;
    WCHAR Semantic[1024];
    WCHAR SizeText[32];
    WCHAR BlockText[32];
} FTP_DIRECTORY_EVENT_SCRATCH, *PFTP_DIRECTORY_EVENT_SCRATCH;

typedef struct _FTP_TRANSFER_SUMMARY {
    ULONGLONG TotalBytesObserved;
    ULONGLONG TotalBytesMissed;
    ULONGLONG ListedFileBytes;
    ULONG DirectoryEntryCount;
    ULONG DirectoryFileCount;
    ULONG DirectoryDirectoryCount;
    ULONG DirectoryLinkCount;
    ULONG DirectoryEntryEventsSuppressed;
    BOOLEAN DirectoryLineOverflow;
} FTP_TRANSFER_SUMMARY, *PFTP_TRANSFER_SUMMARY;

typedef struct _FTP_CONTENT_BUILD_SCRATCH {
    UCHAR Combined[PS_FTP_CONTENT_PREVIEW_BYTES];
    WCHAR Preview[PS_FTP_CONTENT_PREVIEW_BYTES];
} FTP_CONTENT_BUILD_SCRATCH, *PFTP_CONTENT_BUILD_SCRATCH;

typedef struct _FTP_RESPONSE_SCRATCH {
    WCHAR Response[PS_URL_LEN];
    WCHAR LastCommand[PS_URL_LEN];
    WCHAR PendingTransfer[PS_URL_LEN];
    WCHAR Decorated[384];
    FTP_TRANSFER_SUMMARY TransferSummary;
    FTP_FLOW_CONTROL_INFO ControlInfo;
} FTP_RESPONSE_SCRATCH, *PFTP_RESPONSE_SCRATCH;

typedef struct _JSON_SEMANTIC_PARSE_SCRATCH {
    WCHAR Keys[PS_JSON_SEMANTIC_MAX_DEPTH + 2][PS_JSON_KEY_LEN];
    WCHAR Paths[PS_JSON_SEMANTIC_MAX_DEPTH + 2][PS_JSON_PATH_CHARS];
    WCHAR Values[PS_JSON_SEMANTIC_MAX_DEPTH + 2][PS_JSON_VALUE_LEN];
} JSON_SEMANTIC_PARSE_SCRATCH, *PJSON_SEMANTIC_PARSE_SCRATCH;

static
BOOLEAN
IsFtpTransferCommand(
    _In_z_ PCWSTR Command
    );

static
BOOLEAN
TryEnqueueHttpRequestEvent(
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    );

static
BOOLEAN
TryEnqueueHttpResponseEvent(
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    );

static
VOID
CopyAddressesV4(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _Inout_ PDLP_NET_EVENT Event
    );

static
VOID
CopyAddressesV6(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _Inout_ PDLP_NET_EVENT Event
    );


static
BOOLEAN
ParseDecimalPort(
    _In_reads_(Length) const WCHAR* Text,
    _In_ SIZE_T Length,
    _Out_ UINT16* Port
    )
{
    ULONG value = 0;
    SIZE_T i;

    if (Text == NULL || Length == 0 || Port == NULL) {
        return FALSE;
    }

    for (i = 0; i < Length; i++) {
        if (Text[i] < L'0' || Text[i] > L'9') {
            return FALSE;
        }
        value = value * 10 + (ULONG)(Text[i] - L'0');
        if (value > 65535) {
            return FALSE;
        }
    }

    *Port = (UINT16)value;
    return TRUE;
}

static STREAM_FLOW_CACHE_ENTRY gStreamFlowCache[PS_STREAM_FLOW_CACHE_SIZE];
static KSPIN_LOCK gStreamFlowCacheLock;
static BOOLEAN gStreamFlowCacheInitialized = FALSE;

static FTP_DATA_EXPECTATION gFtpDataExpectations[PS_FTP_DATA_EXPECTATION_SIZE];
static KSPIN_LOCK gFtpDataLock;
static BOOLEAN gFtpDataLockInitialized = FALSE;
static FTP_ACTIVE_FLOW gFtpActiveFlows[PS_FTP_ACTIVE_FLOW_SIZE];
static KSPIN_LOCK gFtpActiveFlowLock;
static BOOLEAN gFtpActiveFlowLockInitialized = FALSE;
static NPAGED_LOOKASIDE_LIST gStreamInspectBufferLookaside;
static NPAGED_LOOKASIDE_LIST gFtpPreviewScratchLookaside;
static BOOLEAN gClassificationInitialized = FALSE;

NTSTATUS
ClassificationInitialize(VOID)
{
    if (gClassificationInitialized) {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(gStreamFlowCache, sizeof(gStreamFlowCache));
    KeInitializeSpinLock(&gStreamFlowCacheLock);
    gStreamFlowCacheInitialized = TRUE;

    RtlZeroMemory(gFtpDataExpectations, sizeof(gFtpDataExpectations));
    KeInitializeSpinLock(&gFtpDataLock);
    gFtpDataLockInitialized = TRUE;

    RtlZeroMemory(gFtpActiveFlows, sizeof(gFtpActiveFlows));
    KeInitializeSpinLock(&gFtpActiveFlowLock);
    gFtpActiveFlowLockInitialized = TRUE;

    ExInitializeNPagedLookasideList(
        &gStreamInspectBufferLookaside,
        NULL,
        NULL,
        POOL_NX_ALLOCATION,
        PS_STREAM_INSPECT_BYTES,
        'bIsP',
        0);
    ExInitializeNPagedLookasideList(
        &gFtpPreviewScratchLookaside,
        NULL,
        NULL,
        POOL_NX_ALLOCATION,
        sizeof(FTP_DATA_PREVIEW_SCRATCH),
        'sFsP',
        0);

    gClassificationInitialized = TRUE;
    return STATUS_SUCCESS;
}

VOID
ClassificationCleanup(VOID)
{
    if (!gClassificationInitialized) {
        return;
    }

    gClassificationInitialized = FALSE;
    ExDeleteNPagedLookasideList(&gFtpPreviewScratchLookaside);
    ExDeleteNPagedLookasideList(&gStreamInspectBufferLookaside);
    gStreamFlowCacheInitialized = FALSE;
    gFtpDataLockInitialized = FALSE;
    gFtpActiveFlowLockInitialized = FALSE;
}

static
VOID
EnsureCachesInitialized(VOID)
{
    if (!gStreamFlowCacheInitialized) {
        RtlZeroMemory(gStreamFlowCache, sizeof(gStreamFlowCache));
        KeInitializeSpinLock(&gStreamFlowCacheLock);
        gStreamFlowCacheInitialized = TRUE;
    }
    if (!gFtpDataLockInitialized) {
        RtlZeroMemory(gFtpDataExpectations, sizeof(gFtpDataExpectations));
        KeInitializeSpinLock(&gFtpDataLock);
        gFtpDataLockInitialized = TRUE;
    }
    if (!gFtpActiveFlowLockInitialized) {
        RtlZeroMemory(gFtpActiveFlows, sizeof(gFtpActiveFlows));
        KeInitializeSpinLock(&gFtpActiveFlowLock);
        gFtpActiveFlowLockInitialized = TRUE;
    }
}

static
PSTREAM_FLOW_CACHE_ENTRY
GetOrCreateFlowEntryLocked(
    _In_ UINT64 FlowHandle
    )
{
    PSTREAM_FLOW_CACHE_ENTRY freeEntry = NULL;
    PSTREAM_FLOW_CACHE_ENTRY oldestEntry = NULL;
    ULONG i;

    for (i = 0; i < PS_STREAM_FLOW_CACHE_SIZE; i++) {
        PSTREAM_FLOW_CACHE_ENTRY entry = &gStreamFlowCache[i];
        if (entry->InUse && entry->FlowHandle == FlowHandle) {
            return entry;
        }
        if (!entry->InUse && freeEntry == NULL) {
            freeEntry = entry;
        }
        if (oldestEntry == NULL || entry->LastSeen.QuadPart < oldestEntry->LastSeen.QuadPart) {
            oldestEntry = entry;
        }
    }

    if (freeEntry == NULL) {
        freeEntry = oldestEntry;
    }

    RtlZeroMemory(freeEntry, sizeof(*freeEntry));
    freeEntry->InUse = TRUE;
    freeEntry->FlowHandle = FlowHandle;
    return freeEntry;
}

static
UINT32
HashWideString(
    _In_z_ PCWSTR Text
    )
{
    UINT32 hash = 2166136261u;

    while (*Text != L'\0') {
        WCHAR ch = *Text++;
        if (ch >= L'A' && ch <= L'Z') {
            ch = (WCHAR)(ch - L'A' + L'a');
        }
        hash ^= (UINT16)ch;
        hash *= 16777619u;
    }

    return hash;
}

static
BOOLEAN
ShouldSuppressDuplicate(
    _In_ UINT64 FlowHandle,
    _In_ STREAM_SIGNAL_KIND Kind,
    _In_ UINT32 Hash
    )
{
    KIRQL oldIrql;
    PSTREAM_FLOW_CACHE_ENTRY entry;
    LARGE_INTEGER now;
    UINT32* lastHash;
    LARGE_INTEGER* lastTime;
    BOOLEAN suppress;

    if (FlowHandle == 0) {
        return FALSE;
    }

    EnsureCachesInitialized();
    KeQuerySystemTime(&now);

    KeAcquireSpinLock(&gStreamFlowCacheLock, &oldIrql);
    entry = GetOrCreateFlowEntryLocked(FlowHandle);
    entry->LastSeen = now;

    switch (Kind) {
        case StreamSignalHttpRequest:
            lastHash = &entry->LastHttpRequestHash;
            lastTime = &entry->LastHttpRequestTime;
            break;
        case StreamSignalHttpResponse:
            lastHash = &entry->LastHttpResponseHash;
            lastTime = &entry->LastHttpResponseTime;
            break;
        case StreamSignalSni:
            lastHash = &entry->LastSniHash;
            lastTime = &entry->LastSniTime;
            break;
        default:
            lastHash = &entry->LastFtpLineHash;
            lastTime = &entry->LastFtpLineTime;
            break;
    }

    suppress = (*lastHash == Hash) &&
        (now.QuadPart - lastTime->QuadPart) >= 0 &&
        (now.QuadPart - lastTime->QuadPart) < PS_STREAM_DUP_WINDOW_100NS;

    *lastHash = Hash;
    *lastTime = now;
    KeReleaseSpinLock(&gStreamFlowCacheLock, oldIrql);
    return suppress;
}

static
VOID
CopyAddressBytesV4(
    _In_ UINT32 Address,
    _Out_writes_(4) UCHAR* Buffer
    )
{
    Buffer[0] = (UCHAR)((Address >> 24) & 0xFF);
    Buffer[1] = (UCHAR)((Address >> 16) & 0xFF);
    Buffer[2] = (UCHAR)((Address >> 8) & 0xFF);
    Buffer[3] = (UCHAR)(Address & 0xFF);
}

static
VOID
CopyAddressBytesV6(
    _In_ const FWP_BYTE_ARRAY16* Address,
    _Out_writes_(16) UCHAR* Buffer
    )
{
    if (Address != NULL) {
        RtlCopyMemory(Buffer, Address->byteArray16, 16);
    } else {
        RtlZeroMemory(Buffer, 16);
    }
}

static
BOOLEAN
AddressEquals(
    _In_reads_(Length) const UCHAR* A,
    _In_reads_(Length) const UCHAR* B,
    _In_ ULONG Length
    )
{
    return RtlCompareMemory(A, B, Length) == Length;
}

static
USHORT
FormatIpv4(
    _In_ UINT32 Address,
    _Out_writes_bytes_(BufferSize) UCHAR* Buffer,
    _In_ SIZE_T BufferSize
    )
{
    NTSTATUS status = RtlStringCbPrintfA(
        (char*)Buffer,
        BufferSize,
        "%u.%u.%u.%u",
        (Address >> 24) & 0xFF,
        (Address >> 16) & 0xFF,
        (Address >> 8) & 0xFF,
        Address & 0xFF);

    if (!NT_SUCCESS(status)) {
        Buffer[0] = 0;
        return 0;
    }
    return (USHORT)strlen((char*)Buffer);
}

static
USHORT
FormatIpv6(
    _In_ const FWP_BYTE_ARRAY16* Address,
    _Out_writes_bytes_(BufferSize) UCHAR* Buffer,
    _In_ SIZE_T BufferSize
    )
{
    NTSTATUS status;

    if (Address == NULL || Buffer == NULL || BufferSize == 0) {
        return 0;
    }

    status = RtlStringCbPrintfA(
        (char*)Buffer,
        BufferSize,
        "%x:%x:%x:%x:%x:%x:%x:%x",
        ((USHORT)Address->byteArray16[0] << 8) | Address->byteArray16[1],
        ((USHORT)Address->byteArray16[2] << 8) | Address->byteArray16[3],
        ((USHORT)Address->byteArray16[4] << 8) | Address->byteArray16[5],
        ((USHORT)Address->byteArray16[6] << 8) | Address->byteArray16[7],
        ((USHORT)Address->byteArray16[8] << 8) | Address->byteArray16[9],
        ((USHORT)Address->byteArray16[10] << 8) | Address->byteArray16[11],
        ((USHORT)Address->byteArray16[12] << 8) | Address->byteArray16[13],
        ((USHORT)Address->byteArray16[14] << 8) | Address->byteArray16[15]);
    if (!NT_SUCCESS(status)) {
        Buffer[0] = 0;
        return 0;
    }
    return (USHORT)strlen((char*)Buffer);
}

static
USHORT
CopyProcessNameFromProcessPathBlob(
    _In_opt_ const FWP_BYTE_BLOB* ProcessPath,
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
    )
{
    USHORT charCount;
    USHORT startIndex;
    USHORT copyLen;
    USHORT i;
    const WCHAR* source;

    if (ProcessPath == NULL || ProcessPath->data == NULL ||
        ProcessPath->size < sizeof(WCHAR) || Buffer == NULL || BufferLengthInChars == 0) {
        return 0;
    }

    source = (const WCHAR*)ProcessPath->data;
    charCount = (USHORT)(ProcessPath->size / sizeof(WCHAR));
    startIndex = 0;
    for (i = 0; i < charCount; i++) {
        if (source[i] == L'\0') {
            charCount = i;
            break;
        }
        if (source[i] == L'\\' || source[i] == L'/') {
            startIndex = (USHORT)(i + 1);
        }
    }

    if (startIndex >= charCount) {
        startIndex = 0;
    }

    copyLen = (USHORT)(charCount - startIndex);
    if (copyLen >= BufferLengthInChars) {
        copyLen = (USHORT)(BufferLengthInChars - 1);
    }

    if (copyLen == 0) {
        return 0;
    }

    RtlCopyMemory(Buffer, source + startIndex, copyLen * sizeof(WCHAR));
    Buffer[copyLen] = L'\0';
    return copyLen;
}

static
VOID
FillProcessNameForEvent(
    _Inout_ PDLP_NET_EVENT Event,
    _In_ ULONG ProcessId,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues
    )
{
    Event->ProcessNameLength = 0;
    if (KeGetCurrentIrql() <= APC_LEVEL) {
        Event->ProcessNameLength = ProcessTrackerGetProcessNameById(
            (HANDLE)(ULONG_PTR)ProcessId,
            Event->ProcessName,
            ARRAYSIZE(Event->ProcessName));
    }

    if (Event->ProcessNameLength == 0 &&
        InMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_PATH)) {
        Event->ProcessNameLength = CopyProcessNameFromProcessPathBlob(
            InMetaValues->processPath,
            Event->ProcessName,
            ARRAYSIZE(Event->ProcessName));
    }

    if (Event->ProcessNameLength == 0 && ProcessId != 0 &&
        KeGetCurrentIrql() <= APC_LEVEL) {
        PEPROCESS process = NULL;
        if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)ProcessId, &process))) {
            ProcessTrackerTrackProcess(process);
            Event->ProcessNameLength = ProcessTrackerGetProcessName(
                process,
                Event->ProcessName,
                ARRAYSIZE(Event->ProcessName));
            ObDereferenceObject(process);
        }
    }
}

static
ACTION_RESULT
FinalizeInspectionEvent(
    _Inout_ PDLP_NET_EVENT Event,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ DLP_EVENT_TYPE EventType,
    _In_opt_ PCWSTR ContentValue,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues
    )
{
    Event->EventType = EventType;
    Event->ProcessId = ProcessId;
    Event->Protocol = IPPROTO_TCP;
    Event->RemotePort = RemotePort;
    Event->LocalPort = LocalPort;
    KeQuerySystemTime(&Event->Timestamp);
    FillProcessNameForEvent(Event, ProcessId, InMetaValues);
    Event->ActionResult = PolicyEngineQueryNetAction(ProcessId, RemotePort, ContentValue, EventType);
    return Event->ActionResult;
}

static
VOID
UpdateFlowControlInfo(
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr
    )
{
    KIRQL oldIrql;
    PSTREAM_FLOW_CACHE_ENTRY entry;

    if (FlowHandle == 0) {
        return;
    }

    EnsureCachesInitialized();
    KeAcquireSpinLock(&gStreamFlowCacheLock, &oldIrql);
    entry = GetOrCreateFlowEntryLocked(FlowHandle);
    KeQuerySystemTime(&entry->LastSeen);
    entry->ControlInfoValid = TRUE;
    entry->IsV6 = IsV6;
    entry->ProcessId = ProcessId;
    entry->ControlLocalPort = LocalPort;
    entry->ControlRemotePort = RemotePort;
    RtlCopyMemory(entry->ControlLocalAddress, LocalAddr, 16);
    RtlCopyMemory(entry->ControlRemoteAddress, RemoteAddr, 16);
    KeReleaseSpinLock(&gStreamFlowCacheLock, oldIrql);
}

static
BOOLEAN
GetFlowControlInfo(
    _In_ UINT64 FlowHandle,
    _Out_ PFTP_FLOW_CONTROL_INFO Snapshot
    )
{
    KIRQL oldIrql;
    PSTREAM_FLOW_CACHE_ENTRY entry;
    BOOLEAN found = FALSE;

    if (Snapshot == NULL || FlowHandle == 0) {
        return FALSE;
    }

    EnsureCachesInitialized();
    KeAcquireSpinLock(&gStreamFlowCacheLock, &oldIrql);
    entry = GetOrCreateFlowEntryLocked(FlowHandle);
    if (entry->InUse && entry->FlowHandle == FlowHandle && entry->ControlInfoValid) {
        Snapshot->IsV6 = entry->IsV6;
        RtlCopyMemory(Snapshot->LocalAddress, entry->ControlLocalAddress, sizeof(Snapshot->LocalAddress));
        RtlCopyMemory(Snapshot->RemoteAddress, entry->ControlRemoteAddress, sizeof(Snapshot->RemoteAddress));
        found = TRUE;
    }
    KeReleaseSpinLock(&gStreamFlowCacheLock, oldIrql);
    return found;
}

static
VOID
UpdateFtpSessionState(
    _In_ UINT64 FlowHandle,
    _In_opt_z_ PCWSTR Command,
    _In_ BOOLEAN IsResponse,
    _In_ ULONG ResponseCode
    )
{
    KIRQL oldIrql;
    PSTREAM_FLOW_CACHE_ENTRY entry;

    if (FlowHandle == 0) {
        return;
    }

    EnsureCachesInitialized();

    KeAcquireSpinLock(&gStreamFlowCacheLock, &oldIrql);
    entry = GetOrCreateFlowEntryLocked(FlowHandle);
    KeQuerySystemTime(&entry->LastSeen);

    if (!IsResponse && Command != NULL) {
        RtlStringCchCopyW(entry->LastFtpCommand, ARRAYSIZE(entry->LastFtpCommand), Command);
        if (IsFtpTransferCommand(Command)) {
            RtlStringCchCopyW(entry->PendingTransferCommand, ARRAYSIZE(entry->PendingTransferCommand), Command);
            entry->TransferInProgress = FALSE;
            entry->TransferDataSeen = FALSE;
        } else if (_wcsnicmp(Command, L"ABOR", 4) == 0 || _wcsnicmp(Command, L"QUIT", 4) == 0) {
            entry->PendingTransferCommand[0] = L'\0';
            entry->TransferInProgress = FALSE;
            entry->TransferDataSeen = FALSE;
        }
    } else if (IsResponse) {
        if (ResponseCode == 230) {
            entry->FtpAuthenticated = TRUE;
        } else if (ResponseCode == 221 || ResponseCode == 421 || ResponseCode == 530) {
            entry->FtpAuthenticated = FALSE;
        }

        if (ResponseCode == 125 || ResponseCode == 150) {
            entry->TransferInProgress = TRUE;
        } else if (ResponseCode == 226 || ResponseCode == 250) {
            entry->PendingTransferCommand[0] = L'\0';
            entry->TransferInProgress = FALSE;
            entry->TransferDataSeen = FALSE;
        } else if (ResponseCode == 425 || ResponseCode == 426 ||
                   ResponseCode == 450 || ResponseCode == 451 ||
                   ResponseCode == 452 || ResponseCode == 550 ||
                   ResponseCode == 551 || ResponseCode == 552 ||
                   ResponseCode == 553) {
            entry->PendingTransferCommand[0] = L'\0';
            entry->TransferInProgress = FALSE;
            entry->TransferDataSeen = FALSE;
        }
    }

    KeReleaseSpinLock(&gStreamFlowCacheLock, oldIrql);
}

static
BOOLEAN
GetFtpSessionState(
    _In_ UINT64 FlowHandle,
    _Out_writes_to_(CommandBufferChars, return) PWCHAR LastCommand,
    _In_ USHORT CommandBufferChars,
    _Out_ PBOOLEAN Authenticated,
    _Out_writes_to_opt_(PendingChars, return) PWCHAR PendingTransfer,
    _In_ USHORT PendingChars,
    _Out_opt_ PBOOLEAN TransferInProgress,
    _Out_opt_ PBOOLEAN TransferDataSeen
    )
{
    KIRQL oldIrql;
    PSTREAM_FLOW_CACHE_ENTRY entry;
    BOOLEAN found = FALSE;

    if (LastCommand != NULL && CommandBufferChars > 0) {
        LastCommand[0] = L'\0';
    }
    if (Authenticated != NULL) {
        *Authenticated = FALSE;
    }
    if (PendingTransfer != NULL && PendingChars > 0) {
        PendingTransfer[0] = L'\0';
    }
    if (TransferInProgress != NULL) {
        *TransferInProgress = FALSE;
    }
    if (TransferDataSeen != NULL) {
        *TransferDataSeen = FALSE;
    }

    if (FlowHandle == 0) {
        return FALSE;
    }

    EnsureCachesInitialized();

    KeAcquireSpinLock(&gStreamFlowCacheLock, &oldIrql);
    entry = GetOrCreateFlowEntryLocked(FlowHandle);
    if (entry->InUse && entry->FlowHandle == FlowHandle) {
        found = TRUE;
        if (LastCommand != NULL && CommandBufferChars > 0) {
            RtlStringCchCopyW(LastCommand, CommandBufferChars, entry->LastFtpCommand);
        }
        if (Authenticated != NULL) {
            *Authenticated = entry->FtpAuthenticated;
        }
        if (PendingTransfer != NULL && PendingChars > 0) {
            RtlStringCchCopyW(PendingTransfer, PendingChars, entry->PendingTransferCommand);
        }
        if (TransferInProgress != NULL) {
            *TransferInProgress = entry->TransferInProgress;
        }
        if (TransferDataSeen != NULL) {
            *TransferDataSeen = entry->TransferDataSeen;
        }
    }
    KeReleaseSpinLock(&gStreamFlowCacheLock, oldIrql);

    return found;
}

static
VOID
RegisterFtpDataExpectation(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ BOOLEAN OutboundExpected,
    _In_ UINT16 ExpectedLocalPort,
    _In_ UINT16 ExpectedRemotePort,
    _In_reads_opt_(16) const UCHAR* LocalAddr,
    _In_reads_opt_(16) const UCHAR* RemoteAddr,
    _In_opt_z_ PCWSTR Command
    )
{
    KIRQL oldIrql;
    PFTP_DATA_EXPECTATION freeEntry = NULL;
    PFTP_DATA_EXPECTATION oldestEntry = NULL;
    LARGE_INTEGER now;
    ULONG i;

    EnsureCachesInitialized();
    KeQuerySystemTime(&now);

    KeAcquireSpinLock(&gFtpDataLock, &oldIrql);
    for (i = 0; i < PS_FTP_DATA_EXPECTATION_SIZE; i++) {
        PFTP_DATA_EXPECTATION entry = &gFtpDataExpectations[i];
        if (!entry->InUse && freeEntry == NULL) {
            freeEntry = entry;
        }
        if (oldestEntry == NULL || entry->Created.QuadPart < oldestEntry->Created.QuadPart) {
            oldestEntry = entry;
        }
    }

    if (freeEntry == NULL) {
        freeEntry = oldestEntry;
    }

    RtlZeroMemory(freeEntry, sizeof(*freeEntry));
    freeEntry->InUse = TRUE;
    freeEntry->ProcessId = ProcessId;
    freeEntry->IsV6 = IsV6;
    freeEntry->OutboundExpected = OutboundExpected;
    freeEntry->ExpectedLocalPort = ExpectedLocalPort;
    freeEntry->ExpectedRemotePort = ExpectedRemotePort;
    freeEntry->Created = now;
    freeEntry->HasLocalAddress = (LocalAddr != NULL);
    freeEntry->HasRemoteAddress = (RemoteAddr != NULL);
    if (LocalAddr != NULL) {
        RtlCopyMemory(freeEntry->LocalAddress, LocalAddr, 16);
    }
    if (RemoteAddr != NULL) {
        RtlCopyMemory(freeEntry->RemoteAddress, RemoteAddr, 16);
    }
    if (Command != NULL) {
        RtlStringCchCopyW(freeEntry->Command, ARRAYSIZE(freeEntry->Command), Command);
    }
    KeReleaseSpinLock(&gFtpDataLock, oldIrql);
}

static
VOID
UpdateFtpDataExpectationCommand(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_z_ PCWSTR Command
    )
{
    KIRQL oldIrql;
    PFTP_DATA_EXPECTATION newestEntry = NULL;
    ULONG i;

    if (!IsFtpTransferCommand(Command)) {
        return;
    }

    EnsureCachesInitialized();
    KeAcquireSpinLock(&gFtpDataLock, &oldIrql);
    for (i = 0; i < PS_FTP_DATA_EXPECTATION_SIZE; i++) {
        PFTP_DATA_EXPECTATION entry = &gFtpDataExpectations[i];
        if (!entry->InUse || entry->ProcessId != ProcessId || entry->IsV6 != IsV6) {
            continue;
        }
        if (entry->HasRemoteAddress &&
            !AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            continue;
        }
        if (newestEntry == NULL || entry->Created.QuadPart > newestEntry->Created.QuadPart) {
            newestEntry = entry;
        }
    }
    if (newestEntry != NULL) {
        RtlStringCchCopyW(newestEntry->Command, ARRAYSIZE(newestEntry->Command), Command);
    }
    KeReleaseSpinLock(&gFtpDataLock, oldIrql);
}

static
VOID
UpdateFtpActiveFlowCommand(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_z_ PCWSTR Command
    )
{
    KIRQL oldIrql;
    PFTP_ACTIVE_FLOW newestEntry = NULL;
    ULONG i;

    if (!IsFtpTransferCommand(Command)) {
        return;
    }

    EnsureCachesInitialized();
    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW entry = &gFtpActiveFlows[i];
        if (!entry->InUse || entry->ProcessId != ProcessId || entry->IsV6 != IsV6 ||
            !AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            continue;
        }
        if (newestEntry == NULL || entry->Created.QuadPart > newestEntry->Created.QuadPart) {
            newestEntry = entry;
        }
    }
    if (newestEntry != NULL) {
        RtlStringCchCopyW(newestEntry->Command, ARRAYSIZE(newestEntry->Command), Command);
        newestEntry->TotalBytesObserved = 0;
        newestEntry->TotalBytesMissed = 0;
        newestEntry->ListedFileBytes = 0;
        newestEntry->DirectoryEntryCount = 0;
        newestEntry->DirectoryFileCount = 0;
        newestEntry->DirectoryDirectoryCount = 0;
        newestEntry->DirectoryLinkCount = 0;
        newestEntry->DirectoryEntryEventsSuppressed = 0;
        newestEntry->DirectoryLineBytes = 0;
        newestEntry->DirectoryLineOverflow = FALSE;
        newestEntry->DirectoryHadTruncatedLine = FALSE;
        newestEntry->ContentTailBytes = 0;
        newestEntry->DetectedContentType[0] = L'\0';
    }
    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
}

static
VOID
RegisterFtpActiveFlow(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ BOOLEAN OutboundObserved,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_opt_z_ PCWSTR Command
    )
{
    KIRQL oldIrql;
    PFTP_ACTIVE_FLOW freeEntry = NULL;
    PFTP_ACTIVE_FLOW oldestEntry = NULL;
    LARGE_INTEGER now;
    ULONG i;

    EnsureCachesInitialized();
    KeQuerySystemTime(&now);

    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW entry = &gFtpActiveFlows[i];
        if (entry->InUse &&
            entry->ProcessId == ProcessId &&
            entry->IsV6 == IsV6 &&
            entry->OutboundObserved == OutboundObserved &&
            entry->LocalPort == LocalPort &&
            entry->RemotePort == RemotePort &&
            AddressEquals(entry->LocalAddress, LocalAddr, IsV6 ? 16u : 4u) &&
            AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            if (Command != NULL) {
                RtlStringCchCopyW(entry->Command, ARRAYSIZE(entry->Command), Command);
            }
            entry->LastSeen = now;
            KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
            return;
        }
        if (!entry->InUse && freeEntry == NULL) {
            freeEntry = entry;
        }
        if (oldestEntry == NULL || entry->Created.QuadPart < oldestEntry->Created.QuadPart) {
            oldestEntry = entry;
        }
    }

    if (freeEntry == NULL) {
        freeEntry = oldestEntry;
    }

    RtlZeroMemory(freeEntry, sizeof(*freeEntry));
    freeEntry->InUse = TRUE;
    freeEntry->ProcessId = ProcessId;
    freeEntry->IsV6 = IsV6;
    freeEntry->OutboundObserved = OutboundObserved;
    freeEntry->LocalPort = LocalPort;
    freeEntry->RemotePort = RemotePort;
    RtlCopyMemory(freeEntry->LocalAddress, LocalAddr, 16);
    RtlCopyMemory(freeEntry->RemoteAddress, RemoteAddr, 16);
    if (Command != NULL) {
        RtlStringCchCopyW(freeEntry->Command, ARRAYSIZE(freeEntry->Command), Command);
    }
    freeEntry->Created = now;
    freeEntry->LastSeen = now;
    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
}

static
BOOLEAN
FindFtpActiveFlow(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ BOOLEAN OutboundObserved,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _Out_writes_to_(CommandChars, return) PWCHAR Command,
    _In_ USHORT CommandChars
    )
{
    KIRQL oldIrql;
    LARGE_INTEGER now;
    BOOLEAN found = FALSE;
    ULONG i;

    UNREFERENCED_PARAMETER(OutboundObserved);

    if (Command != NULL && CommandChars > 0) {
        Command[0] = L'\0';
    }

    EnsureCachesInitialized();
    KeQuerySystemTime(&now);

    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW entry = &gFtpActiveFlows[i];
        if (!entry->InUse) {
            continue;
        }
        if ((now.QuadPart - entry->LastSeen.QuadPart) > 600000000LL) {
            entry->InUse = FALSE;
            continue;
        }
        if (entry->ProcessId != ProcessId || entry->IsV6 != IsV6 ||
            entry->LocalPort != LocalPort || entry->RemotePort != RemotePort) {
            continue;
        }
        if (!AddressEquals(entry->LocalAddress, LocalAddr, IsV6 ? 16u : 4u) ||
            !AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            continue;
        }

        if (Command != NULL && CommandChars > 0) {
            RtlStringCchCopyW(Command, CommandChars, entry->Command);
        }
        found = TRUE;
        break;
    }
    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
    return found;
}

static
VOID
UpdateFtpActiveFlowBytes(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ BOOLEAN OutboundObserved,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_ ULONG BytesObserved
    )
{
    KIRQL oldIrql;
    LARGE_INTEGER now;
    ULONG i;

    UNREFERENCED_PARAMETER(OutboundObserved);

    EnsureCachesInitialized();
    KeQuerySystemTime(&now);

    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW entry = &gFtpActiveFlows[i];
        if (!entry->InUse) {
            continue;
        }
        if (entry->ProcessId != ProcessId || entry->IsV6 != IsV6 ||
            entry->LocalPort != LocalPort || entry->RemotePort != RemotePort) {
            continue;
        }
        if (!AddressEquals(entry->LocalAddress, LocalAddr, IsV6 ? 16u : 4u) ||
            !AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            continue;
        }

        entry->TotalBytesObserved += BytesObserved;
        entry->LastSeen = now;
        break;
    }
    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
}

static
VOID
UpdateFtpActiveFlowMissedBytes(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_ SIZE_T BytesMissed
    )
{
    KIRQL oldIrql;
    LARGE_INTEGER now;
    ULONG i;

    if (BytesMissed == 0) return;
    EnsureCachesInitialized();
    KeQuerySystemTime(&now);
    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW entry = &gFtpActiveFlows[i];
        if (!entry->InUse || entry->ProcessId != ProcessId || entry->IsV6 != IsV6 ||
            entry->LocalPort != LocalPort || entry->RemotePort != RemotePort ||
            !AddressEquals(entry->LocalAddress, LocalAddr, IsV6 ? 16u : 4u) ||
            !AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            continue;
        }
        entry->TotalBytesMissed += (ULONGLONG)BytesMissed;
        entry->LastSeen = now;
        break;
    }
    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
}

static
VOID
QueryAndClearFtpTransferSummary(
    _In_ ULONG ProcessId,
    _In_opt_z_ PCWSTR Command,
    _In_ BOOLEAN ClearEntries,
    _Out_ PFTP_TRANSFER_SUMMARY Summary
    )
{
    KIRQL oldIrql;
    ULONG i;

    if (Summary == NULL) {
        return;
    }
    RtlZeroMemory(Summary, sizeof(*Summary));
    if (Command == NULL || Command[0] == L'\0') {
        return;
    }

    EnsureCachesInitialized();
    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW entry = &gFtpActiveFlows[i];
        if (!entry->InUse || entry->ProcessId != ProcessId) {
            continue;
        }
        if (_wcsicmp(entry->Command, Command) != 0) {
            continue;
        }

        Summary->TotalBytesObserved += entry->TotalBytesObserved;
        Summary->TotalBytesMissed += entry->TotalBytesMissed;
        Summary->ListedFileBytes += entry->ListedFileBytes;
        Summary->DirectoryEntryCount += entry->DirectoryEntryCount;
        Summary->DirectoryFileCount += entry->DirectoryFileCount;
        Summary->DirectoryDirectoryCount += entry->DirectoryDirectoryCount;
        Summary->DirectoryLinkCount += entry->DirectoryLinkCount;
        Summary->DirectoryEntryEventsSuppressed += entry->DirectoryEntryEventsSuppressed;
        Summary->DirectoryLineOverflow = Summary->DirectoryLineOverflow ||
            entry->DirectoryHadTruncatedLine || entry->DirectoryLineOverflow || entry->DirectoryLineBytes != 0;
        if (ClearEntries) {
            entry->InUse = FALSE;
        }
    }
    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
}

static
BOOLEAN
MatchFtpDataExpectation(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ BOOLEAN OutboundObserved,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _Out_writes_to_(CommandChars, return) PWCHAR Command,
    _In_ USHORT CommandChars
    )
{
    KIRQL oldIrql;
    LARGE_INTEGER now;
    BOOLEAN matched = FALSE;
    ULONG i;

    if (Command != NULL && CommandChars > 0) {
        Command[0] = L'\0';
    }

    EnsureCachesInitialized();
    KeQuerySystemTime(&now);

    KeAcquireSpinLock(&gFtpDataLock, &oldIrql);
    for (i = 0; i < PS_FTP_DATA_EXPECTATION_SIZE; i++) {
        PFTP_DATA_EXPECTATION entry = &gFtpDataExpectations[i];
        if (!entry->InUse) {
            continue;
        }
        if ((now.QuadPart - entry->Created.QuadPart) > 600000000LL) {
            entry->InUse = FALSE;
            continue;
        }
        if (entry->ProcessId != ProcessId || entry->IsV6 != IsV6 || entry->OutboundExpected != OutboundObserved) {
            continue;
        }
        if (entry->ExpectedLocalPort != 0 && entry->ExpectedLocalPort != LocalPort) {
            continue;
        }
        if (entry->ExpectedRemotePort != 0 && entry->ExpectedRemotePort != RemotePort) {
            continue;
        }
        if (entry->HasLocalAddress && !AddressEquals(entry->LocalAddress, LocalAddr, IsV6 ? 16u : 4u)) {
            continue;
        }
        if (entry->HasRemoteAddress && !AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            continue;
        }

        if (Command != NULL && CommandChars > 0) {
            RtlStringCchCopyW(Command, CommandChars, entry->Command);
        }
        entry->InUse = FALSE;
        matched = TRUE;
        break;
    }
    KeReleaseSpinLock(&gFtpDataLock, oldIrql);

    return matched;
}

static
BOOLEAN
StartsWithHttpMethod(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ ULONG* MethodLength
    )
{
    static const char* methods[] = {
        "GET ", "POST ", "PUT ", "DELETE ", "HEAD ",
        "OPTIONS ", "PATCH ", "TRACE ", "CONNECT "
    };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(methods); i++) {
        SIZE_T len = strlen(methods[i]);
        if (Length >= len && RtlCompareMemory(Buffer, methods[i], len) == len) {
            *MethodLength = (ULONG)len;
            return TRUE;
        }
    }
    return FALSE;
}

static
BOOLEAN
StartsWithHttpResponse(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length
    )
{
    static const char* prefixes[] = { "HTTP/1.", "HTTP/2 " };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(prefixes); i++) {
        SIZE_T len = strlen(prefixes[i]);
        if (Length >= len && RtlCompareMemory(Buffer, prefixes[i], len) == len) {
            return TRUE;
        }
    }
    return FALSE;
}

static
BOOLEAN
ParseAsciiLine(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_writes_to_(LineBufferChars, return) PWCHAR LineBuffer,
    _In_ USHORT LineBufferChars
    )
{
    ULONG lineEnd = 0;
    ULONG i;

    if (Buffer == NULL || Length == 0 || LineBuffer == NULL || LineBufferChars < 2) {
        return FALSE;
    }

    while (lineEnd < Length && Buffer[lineEnd] != '\r' && Buffer[lineEnd] != '\n') {
        if (lineEnd >= (ULONG)(LineBufferChars - 1)) {
            return FALSE;
        }
        lineEnd++;
    }

    if (lineEnd == 0) {
        return FALSE;
    }

    for (i = 0; i < lineEnd; i++) {
        UCHAR ch = Buffer[i];
        if (ch < 0x20 || ch > 0x7E) {
            return FALSE;
        }
        LineBuffer[i] = (WCHAR)ch;
    }
    LineBuffer[lineEnd] = L'\0';
    return TRUE;
}

static
BOOLEAN
ParseFtpCommand(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_writes_to_(CommandBufferChars, return) PWCHAR CommandBuffer,
    _In_ USHORT CommandBufferChars
    )
{
    ULONG i;

    if (!ParseAsciiLine(Buffer, Length, CommandBuffer, CommandBufferChars)) {
        return FALSE;
    }

    if (wcsnlen(CommandBuffer, CommandBufferChars) < 4) {
        return FALSE;
    }

    for (i = 0; CommandBuffer[i] != L'\0'; i++) {
        if (CommandBuffer[i] == L' ') {
            return i >= 3;
        }
        if ((CommandBuffer[i] < L'A' || CommandBuffer[i] > L'Z') &&
            (CommandBuffer[i] < L'a' || CommandBuffer[i] > L'z')) {
            return FALSE;
        }
    }
    return i >= 3 && i <= 4;
}

static
BOOLEAN
IsFtpTransferCommand(
    _In_z_ PCWSTR Command
    )
{
    return _wcsnicmp(Command, L"RETR ", 5) == 0 ||
        _wcsnicmp(Command, L"STOR ", 5) == 0 ||
        _wcsnicmp(Command, L"APPE ", 5) == 0 ||
        _wcsnicmp(Command, L"STOU", 4) == 0 ||
        _wcsnicmp(Command, L"LIST", 4) == 0 ||
        _wcsnicmp(Command, L"NLST", 4) == 0 ||
        _wcsnicmp(Command, L"MLSD", 4) == 0;
}

static
BOOLEAN
IsFtpDirectoryCommand(
    _In_z_ PCWSTR Command
    )
{
    return _wcsnicmp(Command, L"LIST", 4) == 0 ||
        _wcsnicmp(Command, L"NLST", 4) == 0 ||
        _wcsnicmp(Command, L"MLSD", 4) == 0;
}

static
PWSTR
NextFtpDirectoryToken(
    _Inout_ PWSTR* Cursor
    )
{
    PWSTR start;

    while (**Cursor == L' ' || **Cursor == L'\t') {
        (*Cursor)++;
    }
    if (**Cursor == L'\0') {
        return NULL;
    }

    start = *Cursor;
    while (**Cursor != L'\0' && **Cursor != L' ' && **Cursor != L'\t') {
        (*Cursor)++;
    }
    if (**Cursor != L'\0') {
        *(*Cursor)++ = L'\0';
    }
    return start;
}

static
VOID
CopyFtpDirectoryField(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ SIZE_T DestinationChars,
    _In_opt_z_ PCWSTR Source
    )
{
    if (DestinationChars == 0) {
        return;
    }
    Destination[0] = L'\0';
    if (Source != NULL) {
        RtlStringCchCopyW(Destination, DestinationChars, Source);
    }
}

static
BOOLEAN
ParseFtpUnsigned64(
    _In_z_ PCWSTR Text,
    _Out_ PULONGLONG Value
    )
{
    ULONGLONG result = 0;
    ULONG i = 0;

    if (Text == NULL || Text[0] == L'\0' || Value == NULL) {
        return FALSE;
    }
    while (Text[i] != L'\0') {
        ULONGLONG next;
        if (Text[i] < L'0' || Text[i] > L'9') {
            return FALSE;
        }
        next = result * 10 + (ULONGLONG)(Text[i] - L'0');
        if (next < result) {
            return FALSE;
        }
        result = next;
        i++;
    }
    *Value = result;
    return TRUE;
}

static
VOID
DecodeFtpDirectoryText(
    _In_reads_(Length) const UCHAR* Bytes,
    _In_ ULONG Length,
    _Out_writes_(OutputChars) PWCHAR Output,
    _In_ SIZE_T OutputChars
    )
{
    ULONG input = 0;
    SIZE_T output = 0;

    if (OutputChars == 0) {
        return;
    }

    while (input < Length && output + 1 < OutputChars) {
        ULONG codePoint;
        UCHAR first = Bytes[input++];

        if (first < 0x80) {
            codePoint = first;
        } else if ((first & 0xE0) == 0xC0 && input < Length && (Bytes[input] & 0xC0) == 0x80) {
            codePoint = ((ULONG)(first & 0x1F) << 6) | (ULONG)(Bytes[input++] & 0x3F);
            if (codePoint < 0x80) codePoint = L'?';
        } else if ((first & 0xF0) == 0xE0 && input + 1 < Length &&
                   (Bytes[input] & 0xC0) == 0x80 && (Bytes[input + 1] & 0xC0) == 0x80) {
            codePoint = ((ULONG)(first & 0x0F) << 12) |
                ((ULONG)(Bytes[input] & 0x3F) << 6) |
                (ULONG)(Bytes[input + 1] & 0x3F);
            input += 2;
            if (codePoint < 0x800 || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) codePoint = L'?';
        } else {
            codePoint = L'?';
            while (input < Length && (Bytes[input] & 0xC0) == 0x80) input++;
        }

        if (codePoint < 0x20 && codePoint != L'\t') {
            codePoint = L' ';
        }
        Output[output++] = (WCHAR)codePoint;
    }
    Output[output] = L'\0';
}

static
VOID
TrimFtpDirectoryText(
    _Inout_updates_z_(CapacityChars) PWCHAR Text,
    _In_ SIZE_T CapacityChars
    )
{
    SIZE_T length;
    SIZE_T start = 0;

    UNREFERENCED_PARAMETER(CapacityChars);
    length = wcslen(Text);
    while (start < length && (Text[start] == L' ' || Text[start] == L'\t')) start++;
    while (length > start && (Text[length - 1] == L' ' || Text[length - 1] == L'\t')) length--;
    if (start > 0 && length > start) {
        RtlMoveMemory(Text, Text + start, (length - start) * sizeof(WCHAR));
    }
    Text[length - start] = L'\0';
}

static
BOOLEAN
ParseFtpMlsdEntry(
    _In_z_ PCWSTR Text,
    _Out_ PFTP_DIRECTORY_ENTRY Entry,
    _Out_writes_(BufferChars) PWCHAR Buffer,
    _In_ SIZE_T BufferChars
    )
{
    PWSTR facts;
    PWSTR name;
    PWSTR fact;

    RtlStringCchCopyW(Buffer, BufferChars, Text);
    facts = Buffer;
    name = wcschr(facts, L' ');
    if (name == NULL) {
        return FALSE;
    }
    *name++ = L'\0';
    while (*name == L' ' || *name == L'\t') name++;
    if (*name == L'\0') {
        return FALSE;
    }

    CopyFtpDirectoryField(Entry->Format, ARRAYSIZE(Entry->Format), L"mlsd");
    CopyFtpDirectoryField(Entry->Name, ARRAYSIZE(Entry->Name), name);

    fact = facts;
    while (*fact != L'\0') {
        PWSTR factEnd = wcschr(fact, L';');
        PWSTR separator;
        PWSTR value;

        if (factEnd != NULL) *factEnd = L'\0';
        separator = wcschr(fact, L'=');
        if (separator != NULL) {
            *separator = L'\0';
            value = separator + 1;
            if (_wcsicmp(fact, L"type") == 0) {
                if (_wcsicmp(value, L"file") == 0) {
                    Entry->Type = FtpDirectoryEntryFile;
                } else if (_wcsicmp(value, L"dir") == 0 ||
                           _wcsicmp(value, L"cdir") == 0 ||
                           _wcsicmp(value, L"pdir") == 0) {
                    Entry->Type = FtpDirectoryEntryDirectory;
                } else if (_wcsnicmp(value, L"OS.unix=slink", 13) == 0 ||
                           _wcsnicmp(value, L"slink", 5) == 0) {
                    PWSTR target = wcschr(value, L':');
                    Entry->Type = FtpDirectoryEntryLink;
                    if (target != NULL && target[1] != L'\0') {
                        CopyFtpDirectoryField(Entry->LinkTarget, ARRAYSIZE(Entry->LinkTarget), target + 1);
                    }
                }
            } else if (_wcsicmp(fact, L"size") == 0) {
                Entry->HasSize = ParseFtpUnsigned64(value, &Entry->Size);
            } else if (_wcsicmp(fact, L"modify") == 0) {
                CopyFtpDirectoryField(Entry->Modified, ARRAYSIZE(Entry->Modified), value);
            } else if (_wcsicmp(fact, L"perm") == 0 || _wcsicmp(fact, L"UNIX.mode") == 0) {
                CopyFtpDirectoryField(Entry->Permissions, ARRAYSIZE(Entry->Permissions), value);
            } else if (_wcsicmp(fact, L"UNIX.owner") == 0 || _wcsicmp(fact, L"UNIX.ownername") == 0) {
                CopyFtpDirectoryField(Entry->Owner, ARRAYSIZE(Entry->Owner), value);
            } else if (_wcsicmp(fact, L"UNIX.group") == 0 || _wcsicmp(fact, L"UNIX.groupname") == 0) {
                CopyFtpDirectoryField(Entry->Group, ARRAYSIZE(Entry->Group), value);
            }
        }

        if (factEnd == NULL) break;
        fact = factEnd + 1;
    }

    return TRUE;
}

static
BOOLEAN
ParseFtpEplfEntry(
    _In_z_ PCWSTR Text,
    _Out_ PFTP_DIRECTORY_ENTRY Entry,
    _Out_writes_(BufferChars) PWCHAR Buffer,
    _In_ SIZE_T BufferChars
    )
{
    PWSTR facts;
    PWSTR name;
    PWSTR fact;

    if (Text[0] != L'+') return FALSE;
    RtlStringCchCopyW(Buffer, BufferChars, Text);
    facts = Buffer + 1;
    name = wcschr(facts, L'\t');
    if (name == NULL) return FALSE;
    *name++ = L'\0';
    if (*name == L'\0') return FALSE;

    CopyFtpDirectoryField(Entry->Format, ARRAYSIZE(Entry->Format), L"eplf");
    CopyFtpDirectoryField(Entry->Name, ARRAYSIZE(Entry->Name), name);
    fact = facts;
    while (*fact != L'\0') {
        PWSTR factEnd = wcschr(fact, L',');
        if (factEnd != NULL) *factEnd = L'\0';
        if (_wcsicmp(fact, L"/") == 0) {
            Entry->Type = FtpDirectoryEntryDirectory;
        } else if (_wcsicmp(fact, L"r") == 0) {
            Entry->Type = FtpDirectoryEntryFile;
        } else if (fact[0] == L's' && fact[1] != L'\0') {
            Entry->HasSize = ParseFtpUnsigned64(fact + 1, &Entry->Size);
        } else if (fact[0] == L'm' && fact[1] != L'\0') {
            CopyFtpDirectoryField(Entry->Modified, ARRAYSIZE(Entry->Modified), fact + 1);
        }
        if (factEnd == NULL) break;
        fact = factEnd + 1;
    }
    return TRUE;
}

static
BOOLEAN
ParseFtpVmsEntry(
    _In_z_ PCWSTR Text,
    _Out_ PFTP_DIRECTORY_ENTRY Entry,
    _Out_writes_(BufferChars) PWCHAR Buffer,
    _In_ SIZE_T BufferChars
    )
{
    PWSTR cursor;
    PWSTR name;
    PWSTR blocks;
    PWSTR date;
    PWSTR time;
    PWSTR owner;
    PWSTR version;
    PWSTR blockSeparator;
    SIZE_T nameLength;

    if (wcschr(Text, L';') == NULL) return FALSE;
    RtlStringCchCopyW(Buffer, BufferChars, Text);
    cursor = Buffer;
    name = NextFtpDirectoryToken(&cursor);
    blocks = NextFtpDirectoryToken(&cursor);
    date = NextFtpDirectoryToken(&cursor);
    time = NextFtpDirectoryToken(&cursor);
    owner = NextFtpDirectoryToken(&cursor);
    if (name == NULL || blocks == NULL || date == NULL || time == NULL ||
        wcschr(name, L';') == NULL || wcschr(date, L'-') == NULL) {
        return FALSE;
    }

    version = wcschr(name, L';');
    *version = L'\0';
    nameLength = wcslen(name);
    if (nameLength > 4 && _wcsicmp(name + nameLength - 4, L".DIR") == 0) {
        name[nameLength - 4] = L'\0';
        Entry->Type = FtpDirectoryEntryDirectory;
    } else {
        Entry->Type = FtpDirectoryEntryFile;
    }
    CopyFtpDirectoryField(Entry->Format, ARRAYSIZE(Entry->Format), L"vms");
    CopyFtpDirectoryField(Entry->Name, ARRAYSIZE(Entry->Name), name);
    blockSeparator = wcschr(blocks, L'/');
    if (blockSeparator != NULL) *blockSeparator = L'\0';
    Entry->HasAllocationBlocks = ParseFtpUnsigned64(blocks, &Entry->AllocationBlocks);
    RtlStringCchPrintfW(Entry->Modified, ARRAYSIZE(Entry->Modified), L"%s %s", date, time);
    if (owner != NULL) {
        SIZE_T ownerLength = wcslen(owner);
        if (ownerLength >= 2 && owner[0] == L'[' && owner[ownerLength - 1] == L']') {
            owner[ownerLength - 1] = L'\0';
            owner++;
        }
        CopyFtpDirectoryField(Entry->Owner, ARRAYSIZE(Entry->Owner), owner);
    }
    while (*cursor == L' ' || *cursor == L'\t') cursor++;
    CopyFtpDirectoryField(Entry->Permissions, ARRAYSIZE(Entry->Permissions), cursor);
    return Entry->Name[0] != L'\0';
}

static
BOOLEAN
ParseFtpDirectoryEntry(
    _In_reads_(LineLength) const UCHAR* Line,
    _In_ ULONG LineLength,
    _In_z_ PCWSTR Command,
    _Out_ PFTP_DIRECTORY_ENTRY Entry
    );

static
BOOLEAN
ParseFtpDirectoryEntryWithScratch(
    _In_reads_(LineLength) const UCHAR* Line,
    _In_ ULONG LineLength,
    _In_z_ PCWSTR Command,
    _Out_ PFTP_DIRECTORY_ENTRY Entry,
    _Inout_ PFTP_DIRECTORY_PARSE_SCRATCH Scratch
    )
{
    PWSTR cursor;
    PWSTR token[8];
    PWSTR arrow;
    ULONG i;

    RtlZeroMemory(Entry, sizeof(*Entry));
    DecodeFtpDirectoryText(Line, LineLength, Scratch->Text, ARRAYSIZE(Scratch->Text));
    TrimFtpDirectoryText(Scratch->Text, ARRAYSIZE(Scratch->Text));
    if (Scratch->Text[0] == L'\0' || _wcsnicmp(Scratch->Text, L"total ", 6) == 0) {
        return FALSE;
    }

    if (_wcsnicmp(Command, L"MLSD", 4) == 0) {
        return ParseFtpMlsdEntry(
            Scratch->Text,
            Entry,
            Scratch->ParseBuffer,
            ARRAYSIZE(Scratch->ParseBuffer));
    }

    if (_wcsnicmp(Command, L"NLST", 4) == 0) {
        CopyFtpDirectoryField(Entry->Format, ARRAYSIZE(Entry->Format), L"nlst");
        CopyFtpDirectoryField(Entry->Name, ARRAYSIZE(Entry->Name), Scratch->Text);
        return TRUE;
    }

    if (ParseFtpEplfEntry(
            Scratch->Text,
            Entry,
            Scratch->ParseBuffer,
            ARRAYSIZE(Scratch->ParseBuffer)) ||
        ParseFtpVmsEntry(
            Scratch->Text,
            Entry,
            Scratch->ParseBuffer,
            ARRAYSIZE(Scratch->ParseBuffer))) {
        return TRUE;
    }

    RtlStringCchCopyW(Scratch->ParseBuffer, ARRAYSIZE(Scratch->ParseBuffer), Scratch->Text);
    cursor = Scratch->ParseBuffer;
    if ((cursor[0] == L'-' || cursor[0] == L'd' || cursor[0] == L'l' ||
         cursor[0] == L'b' || cursor[0] == L'c' || cursor[0] == L'p' || cursor[0] == L's') &&
        wcslen(cursor) >= 10 && (cursor[10] == L' ' || cursor[10] == L'\t')) {
        for (i = 0; i < ARRAYSIZE(token); i++) {
            token[i] = NextFtpDirectoryToken(&cursor);
            if (token[i] == NULL) break;
        }
        while (*cursor == L' ' || *cursor == L'\t') cursor++;
        if (i == ARRAYSIZE(token) && *cursor != L'\0') {
            CopyFtpDirectoryField(Entry->Format, ARRAYSIZE(Entry->Format), L"unix");
            Entry->Type = token[0][0] == L'd' ? FtpDirectoryEntryDirectory :
                (token[0][0] == L'l' ? FtpDirectoryEntryLink : FtpDirectoryEntryFile);
            CopyFtpDirectoryField(Entry->Permissions, ARRAYSIZE(Entry->Permissions), token[0]);
            CopyFtpDirectoryField(Entry->Owner, ARRAYSIZE(Entry->Owner), token[2]);
            CopyFtpDirectoryField(Entry->Group, ARRAYSIZE(Entry->Group), token[3]);
            Entry->HasSize = ParseFtpUnsigned64(token[4], &Entry->Size);
            RtlStringCchPrintfW(Entry->Modified, ARRAYSIZE(Entry->Modified), L"%s %s %s", token[5], token[6], token[7]);
            if (Entry->Type == FtpDirectoryEntryLink && (arrow = wcsstr(cursor, L" -> ")) != NULL) {
                *arrow = L'\0';
                CopyFtpDirectoryField(Entry->LinkTarget, ARRAYSIZE(Entry->LinkTarget), arrow + 4);
            }
            CopyFtpDirectoryField(Entry->Name, ARRAYSIZE(Entry->Name), cursor);
            return Entry->Name[0] != L'\0';
        }
    }

    RtlStringCchCopyW(Scratch->ParseBuffer, ARRAYSIZE(Scratch->ParseBuffer), Scratch->Text);
    cursor = Scratch->ParseBuffer;
    token[0] = NextFtpDirectoryToken(&cursor);
    token[1] = NextFtpDirectoryToken(&cursor);
    token[2] = NextFtpDirectoryToken(&cursor);
    while (*cursor == L' ' || *cursor == L'\t') cursor++;
    if (token[0] != NULL && token[1] != NULL && token[2] != NULL && *cursor != L'\0' &&
        wcslen(token[0]) >= 8 &&
        (token[0][2] == L'-' || token[0][2] == L'/') &&
        (token[0][5] == L'-' || token[0][5] == L'/')) {
        CopyFtpDirectoryField(Entry->Format, ARRAYSIZE(Entry->Format), L"dos");
        Entry->Type = _wcsicmp(token[2], L"<DIR>") == 0
            ? FtpDirectoryEntryDirectory : FtpDirectoryEntryFile;
        if (Entry->Type == FtpDirectoryEntryFile) {
            Entry->HasSize = ParseFtpUnsigned64(token[2], &Entry->Size);
        }
        RtlStringCchPrintfW(Entry->Modified, ARRAYSIZE(Entry->Modified), L"%s %s", token[0], token[1]);
        CopyFtpDirectoryField(Entry->Name, ARRAYSIZE(Entry->Name), cursor);
        return Entry->Name[0] != L'\0';
    }

    CopyFtpDirectoryField(Entry->Format, ARRAYSIZE(Entry->Format), L"raw");
    CopyFtpDirectoryField(Entry->Name, ARRAYSIZE(Entry->Name), Scratch->Text);
    return TRUE;
}

static
BOOLEAN
ParseFtpDirectoryEntry(
    _In_reads_(LineLength) const UCHAR* Line,
    _In_ ULONG LineLength,
    _In_z_ PCWSTR Command,
    _Out_ PFTP_DIRECTORY_ENTRY Entry
    )
{
    PFTP_DIRECTORY_PARSE_SCRATCH scratch;
    BOOLEAN parsed;

    scratch = (PFTP_DIRECTORY_PARSE_SCRATCH)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*scratch),
        'pFsP');
    if (scratch == NULL) {
        return FALSE;
    }

    parsed = ParseFtpDirectoryEntryWithScratch(Line, LineLength, Command, Entry, scratch);
    ExFreePool(scratch);
    return parsed;
}

static
BOOLEAN
ParseFtpResponse(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_writes_to_(ResponseBufferChars, return) PWCHAR ResponseBuffer,
    _In_ USHORT ResponseBufferChars,
    _Out_ PULONG ResponseCode
    )
{
    if (!ParseAsciiLine(Buffer, Length, ResponseBuffer, ResponseBufferChars)) {
        return FALSE;
    }

    if (wcslen(ResponseBuffer) < 4 ||
        ResponseBuffer[0] < L'0' || ResponseBuffer[0] > L'9' ||
        ResponseBuffer[1] < L'0' || ResponseBuffer[1] > L'9' ||
        ResponseBuffer[2] < L'0' || ResponseBuffer[2] > L'9' ||
        (ResponseBuffer[3] != L' ' && ResponseBuffer[3] != L'-')) {
        return FALSE;
    }

    *ResponseCode =
        (ULONG)(ResponseBuffer[0] - L'0') * 100 +
        (ULONG)(ResponseBuffer[1] - L'0') * 10 +
        (ULONG)(ResponseBuffer[2] - L'0');
    return TRUE;
}

static
BOOLEAN
ParseFtpPortCommand(
    _In_z_ PCWSTR Command,
    _Out_ UINT16* Port
    )
{
    ULONG values[6];
    ULONG index = 0;
    ULONG current = 0;
    BOOLEAN hasDigit = FALSE;
    const WCHAR* p = Command;

    if (wcsncmp(Command, L"PORT ", 5) != 0) {
        return FALSE;
    }
    p += 5;

    while (*p != L'\0' && index < 6) {
        if (*p >= L'0' && *p <= L'9') {
            hasDigit = TRUE;
            current = current * 10 + (ULONG)(*p - L'0');
            if (current > 255) return FALSE;
        } else if (*p == L',' && hasDigit) {
            values[index++] = current;
            current = 0;
            hasDigit = FALSE;
        } else {
            return FALSE;
        }
        p++;
    }

    if (index != 5 || !hasDigit) {
        return FALSE;
    }
    values[index] = current;
    *Port = (UINT16)((values[4] << 8) | values[5]);
    return TRUE;
}

static
BOOLEAN
ParseFtpEprtCommand(
    _In_z_ PCWSTR Command,
    _Out_ UINT16* Port
    )
{
    const WCHAR* p = wcsstr(Command, L"EPRT ");
    if (p == NULL) return FALSE;
    p += 5;
    if (*p == L'\0') return FALSE;
    WCHAR delim = *p++;
    const WCHAR* second = wcschr(p, delim);
    if (second == NULL) return FALSE;
    second = wcschr(second + 1, delim);
    if (second == NULL) return FALSE;
    const WCHAR* third = wcschr(second + 1, delim);
    if (third == NULL || third == second + 1) return FALSE;
    return ParseDecimalPort(second + 1, (SIZE_T)(third - (second + 1)), Port) && *Port != 0;
}

static
BOOLEAN
ParseFtpPasvResponse(
    _In_z_ PCWSTR Response,
    _Out_ UINT16* Port
    )
{
    const WCHAR* open = wcschr(Response, L'(');
    ULONG values[6];
    ULONG index = 0;
    ULONG current = 0;
    BOOLEAN hasDigit = FALSE;

    if (open == NULL) return FALSE;
    open++;

    while (*open != L'\0' && *open != L')' && index < 6) {
        if (*open >= L'0' && *open <= L'9') {
            hasDigit = TRUE;
            current = current * 10 + (ULONG)(*open - L'0');
            if (current > 255) return FALSE;
        } else if (*open == L',' && hasDigit) {
            values[index++] = current;
            current = 0;
            hasDigit = FALSE;
        } else {
            return FALSE;
        }
        open++;
    }

    if (index != 5 || !hasDigit) return FALSE;
    values[index] = current;
    *Port = (UINT16)((values[4] << 8) | values[5]);
    return TRUE;
}

static
BOOLEAN
ParseFtpEpsvResponse(
    _In_z_ PCWSTR Response,
    _Out_ UINT16* Port
    )
{
    const WCHAR* open = wcschr(Response, L'(');
    const WCHAR* lastBar;
    if (open == NULL) return FALSE;
    lastBar = wcsrchr(open, L'|');
    if (lastBar == NULL || lastBar == open) return FALSE;
    while (lastBar > open && *(lastBar - 1) == L'|') {
        lastBar--;
    }
    {
        const WCHAR* start = lastBar;
        const WCHAR* end;
        while (*start == L'|') start++;
        end = wcschr(start, L'|');
        if (end == NULL || end == start) return FALSE;
        return ParseDecimalPort(start, (SIZE_T)(end - start), Port) && *Port != 0;
    }
}

static
BOOLEAN
FindHeaderValue(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _In_z_ const char* HeaderName,
    _Out_ const UCHAR** ValueStart,
    _Out_ ULONG* ValueLength
    )
{
    SIZE_T headerNameLen = strlen(HeaderName);
    ULONG i;

    for (i = 0; i + headerNameLen < Length; i++) {
        if ((i == 0 || Buffer[i - 1] == '\n') &&
            _strnicmp((const char*)(Buffer + i), HeaderName, headerNameLen) == 0) {
            ULONG start = i + (ULONG)headerNameLen;
            ULONG end = start;

            while (start < Length && (Buffer[start] == ' ' || Buffer[start] == '\t')) {
                start++;
            }
            while (end < Length && Buffer[end] != '\r' && Buffer[end] != '\n') {
                end++;
            }

            *ValueStart = Buffer + start;
            *ValueLength = end > start ? (end - start) : 0;
            return TRUE;
        }
    }
    return FALSE;
}

static
BOOLEAN
FindHttpHeaderEnd(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ PULONG HeaderBytes
    )
{
    ULONG i;

    if (Buffer == NULL || HeaderBytes == NULL || Length < 4) {
        return FALSE;
    }

    for (i = 0; i + 3 < Length; i++) {
        if (Buffer[i] == '\r' && Buffer[i + 1] == '\n' &&
            Buffer[i + 2] == '\r' && Buffer[i + 3] == '\n') {
            *HeaderBytes = i + 4;
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
FindCrlfLineEnd(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ PULONG LineBytes
    )
{
    ULONG i;

    if (Buffer == NULL || LineBytes == NULL || Length < 2) {
        return FALSE;
    }

    for (i = 0; i + 1 < Length; i++) {
        if (Buffer[i] == '\r' && Buffer[i + 1] == '\n') {
            *LineBytes = i + 2;
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
ParseHttpChunkSizeLine(
    _In_reads_(LineBytes) const UCHAR* Buffer,
    _In_ ULONG LineBytes,
    _Out_ PULONG ChunkSize
    )
{
    ULONG value = 0;
    ULONG i = 0;

    if (Buffer == NULL || ChunkSize == NULL || LineBytes < 2) {
        return FALSE;
    }

    while (i + 1 < LineBytes && Buffer[i] != ';' && Buffer[i] != '\r') {
        UCHAR ch = Buffer[i];
        value <<= 4;
        if (ch >= '0' && ch <= '9') {
            value += (ULONG)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            value += (ULONG)(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            value += (ULONG)(ch - 'A' + 10);
        } else {
            return FALSE;
        }
        i++;
    }

    if (i == 0 || Buffer[LineBytes - 2] != '\r' || Buffer[LineBytes - 1] != '\n') {
        return FALSE;
    }

    *ChunkSize = value;
    return TRUE;
}

static
BOOLEAN
ParseHttpContentLength(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ContentLength
    )
{
    const UCHAR* valueStart = NULL;
    ULONG valueLength = 0;
    ULONG value = 0;
    ULONG i;

    if (!FindHeaderValue(Buffer, Length, "Content-Length:", &valueStart, &valueLength)) {
        return FALSE;
    }

    for (i = 0; i < valueLength; i++) {
        if (valueStart[i] < '0' || valueStart[i] > '9') {
            return FALSE;
        }
        value = value * 10 + (ULONG)(valueStart[i] - '0');
    }

    *ContentLength = value;
    return TRUE;
}

static
BOOLEAN
ParseHttpChunked(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length
    )
{
    const UCHAR* valueStart = NULL;
    ULONG valueLength = 0;

    if (!FindHeaderValue(Buffer, Length, "Transfer-Encoding:", &valueStart, &valueLength)) {
        return FALSE;
    }

    if (valueLength < 7) {
        return FALSE;
    }

    for (ULONG i = 0; i + 7 <= valueLength; i++) {
        if (_strnicmp((const char*)valueStart + i, "chunked", 7) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
FindHttpChunkedTerminator(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ PULONG MessageBytes
    )
{
    ULONG i;

    if (Length < 5) {
        return FALSE;
    }

    for (i = 0; i + 4 < Length; i++) {
        if (Buffer[i] == '0' && Buffer[i + 1] == '\r' &&
            Buffer[i + 2] == '\n' && Buffer[i + 3] == '\r' &&
            Buffer[i + 4] == '\n') {
            *MessageBytes = i + 5;
            return TRUE;
        }
    }

    for (i = 0; i + 6 < Length; i++) {
        if (Buffer[i] == '\r' && Buffer[i + 1] == '\n' &&
            Buffer[i + 2] == '0' && Buffer[i + 3] == '\r' &&
            Buffer[i + 4] == '\n' && Buffer[i + 5] == '\r' &&
            Buffer[i + 6] == '\n') {
            *MessageBytes = i + 7;
            return TRUE;
        }
    }

    return FALSE;
}

static
NTSTATUS
BuildUnicodeUrl(
    _In_reads_opt_(HostLength) const UCHAR* Host,
    _In_ ULONG HostLength,
    _In_reads_(PathLength) const UCHAR* Path,
    _In_ ULONG PathLength,
    _Out_ PWSTR* ParsedUrl,
    _Out_ PULONG ParsedUrlLength
    )
{
    static const WCHAR kHttpPrefix[] = L"http://";
    ULONG prefixLength = HostLength > 0 ? (ULONG)(RTL_NUMBER_OF(kHttpPrefix) - 1) : 0;
    ULONG totalChars = prefixLength + HostLength + PathLength;
    PWSTR url;
    ULONG i;
    ULONG outIndex = 0;

    if (ParsedUrl == NULL || ParsedUrlLength == NULL || Path == NULL || PathLength == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    url = (PWSTR)ExAllocatePoolZero(
        NonPagedPool,
        (SIZE_T)(totalChars + 1) * sizeof(WCHAR),
        'uHsP');
    if (url == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < prefixLength; i++) url[outIndex++] = kHttpPrefix[i];
    for (i = 0; i < HostLength; i++) url[outIndex++] = (WCHAR)Host[i];
    for (i = 0; i < PathLength; i++) url[outIndex++] = (WCHAR)Path[i];
    url[outIndex] = L'\0';

    *ParsedUrl = url;
    *ParsedUrlLength = outIndex * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

static
BOOLEAN
BuildUnicodePreviewLine(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ PWSTR* ParsedLine,
    _Out_ PULONG ParsedLineLength
    )
{
    WCHAR line[256];
    size_t lineLen;
    PWSTR out;

    if (!ParseAsciiLine(Buffer, Length, line, ARRAYSIZE(line))) {
        return FALSE;
    }

    lineLen = wcslen(line);
    out = (PWSTR)ExAllocatePoolZero(NonPagedPool, (lineLen + 1) * sizeof(WCHAR), 'pFsP');
    if (out == NULL) {
        return FALSE;
    }

    RtlCopyMemory(out, line, lineLen * sizeof(WCHAR));
    out[lineLen] = L'\0';
    *ParsedLine = out;
    *ParsedLineLength = (ULONG)(lineLen * sizeof(WCHAR));
    return TRUE;
}

static
BOOLEAN
BuildUnicodeStatusLine(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ PWSTR* ParsedLine,
    _Out_ PULONG ParsedLineLength
    )
{
    WCHAR line[256];
    size_t lineLen;
    PWSTR out;

    if (!ParseAsciiLine(Buffer, Length, line, ARRAYSIZE(line))) {
        return FALSE;
    }

    lineLen = wcslen(line);
    out = (PWSTR)ExAllocatePoolZero(NonPagedPool, (lineLen + 1) * sizeof(WCHAR), 'rHsP');
    if (out == NULL) {
        return FALSE;
    }

    RtlCopyMemory(out, line, lineLen * sizeof(WCHAR));
    out[lineLen] = L'\0';
    *ParsedLine = out;
    *ParsedLineLength = (ULONG)(lineLen * sizeof(WCHAR));
    return TRUE;
}

static
VOID
AppendAsciiFieldToWide(
    _In_reads_(ValueLength) const UCHAR* Value,
    _In_ ULONG ValueLength,
    _Inout_updates_(CapacityChars) PWCHAR Buffer,
    _In_ SIZE_T CapacityChars,
    _Inout_ SIZE_T* Offset
    )
{
    ULONG i;

    for (i = 0; i < ValueLength && *Offset + 1 < CapacityChars; i++) {
        Buffer[(*Offset)++] = (WCHAR)Value[i];
    }
    Buffer[*Offset] = L'\0';
}

static
VOID
AppendWideLiteral(
    _In_z_ PCWSTR Literal,
    _Inout_updates_(CapacityChars) PWCHAR Buffer,
    _In_ SIZE_T CapacityChars,
    _Inout_ SIZE_T* Offset
    )
{
    while (*Literal != L'\0' && *Offset + 1 < CapacityChars) {
        Buffer[(*Offset)++] = *Literal++;
    }
    Buffer[*Offset] = L'\0';
}

static
VOID
AppendHeaderIfPresent(
    _In_reads_(HeaderBytes) const UCHAR* Buffer,
    _In_ ULONG HeaderBytes,
    _In_z_ const char* HeaderName,
    _In_z_ PCWSTR Label,
    _Inout_updates_(CapacityChars) PWCHAR Out,
    _In_ SIZE_T CapacityChars,
    _Inout_ SIZE_T* Offset
    )
{
    const UCHAR* valueStart = NULL;
    ULONG valueLength = 0;

    if (FindHeaderValue(Buffer, HeaderBytes, HeaderName, &valueStart, &valueLength)) {
        AppendWideLiteral(L" | ", Out, CapacityChars, Offset);
        AppendWideLiteral(Label, Out, CapacityChars, Offset);
        AppendWideLiteral(L"=", Out, CapacityChars, Offset);
        AppendAsciiFieldToWide(valueStart, valueLength, Out, CapacityChars, Offset);
    }
}

static
BOOLEAN
BuildHttpHeaderSemantic(
    _In_reads_(HeaderBytes) const UCHAR* Buffer,
    _In_ ULONG HeaderBytes,
    _In_z_ PCWSTR Prefix,
    _In_z_ PCWSTR Primary,
    _In_ BOOLEAN IsRequest,
    _Out_ PWSTR* Semantic,
    _Out_ PULONG SemanticLength
    )
{
    SIZE_T offset = 0;
    SIZE_T finalLen;
    PWSTR out;

    out = (PWSTR)ExAllocatePoolZero(NonPagedPool, 1024 * sizeof(WCHAR), 'hHsP');
    if (out == NULL) return FALSE;
    AppendWideLiteral(Prefix, out, 1024, &offset);
    AppendWideLiteral(L" ", out, 1024, &offset);
    AppendWideLiteral(Primary, out, 1024, &offset);

    if (IsRequest) {
        AppendHeaderIfPresent(Buffer, HeaderBytes, "Host:", L"Host", out, 1024, &offset);
        AppendHeaderIfPresent(Buffer, HeaderBytes, "User-Agent:", L"UA", out, 1024, &offset);
        AppendHeaderIfPresent(Buffer, HeaderBytes, "Content-Type:", L"CT", out, 1024, &offset);
        AppendHeaderIfPresent(Buffer, HeaderBytes, "Authorization:", L"Auth", out, 1024, &offset);
    } else {
        AppendHeaderIfPresent(Buffer, HeaderBytes, "Content-Type:", L"CT", out, 1024, &offset);
        AppendHeaderIfPresent(Buffer, HeaderBytes, "Server:", L"Server", out, 1024, &offset);
        AppendHeaderIfPresent(Buffer, HeaderBytes, "Location:", L"Loc", out, 1024, &offset);
    }

    AppendHeaderIfPresent(Buffer, HeaderBytes, "Transfer-Encoding:", L"TE", out, 1024, &offset);
    AppendHeaderIfPresent(Buffer, HeaderBytes, "Trailer:", L"Trailer", out, 1024, &offset);

    finalLen = wcslen(out);
    *Semantic = out;
    *SemanticLength = (ULONG)(finalLen * sizeof(WCHAR));
    return TRUE;
}

static __declspec(noinline)
BOOLEAN
BuildHttpTrailerSemantic(
    _In_reads_(TrailerBytes) const UCHAR* Buffer,
    _In_ ULONG TrailerBytes,
    _Out_ PWSTR* Semantic,
    _Out_ PULONG SemanticLength
    )
{
    SIZE_T offset = 0;
    SIZE_T finalLen;
    PWSTR out;
    ULONG cursor = 0;

    out = (PWSTR)ExAllocatePoolZero(NonPagedPool, 1024 * sizeof(WCHAR), 'tHsP');
    if (out == NULL) return FALSE;
    AppendWideLiteral(L"TRAILER", out, 1024, &offset);

    while (cursor < TrailerBytes) {
        ULONG lineBytes = 0;
        if (!FindCrlfLineEnd(Buffer + cursor, TrailerBytes - cursor, &lineBytes) || lineBytes <= 2) {
            break;
        }

        AppendWideLiteral(L" | ", out, 1024, &offset);
        AppendAsciiFieldToWide(Buffer + cursor, lineBytes - 2, out, 1024, &offset);
        cursor += lineBytes;
    }

    finalLen = wcslen(out);
    *Semantic = out;
    *SemanticLength = (ULONG)(finalLen * sizeof(WCHAR));
    return TRUE;
}

static
BOOLEAN
ShouldCaptureHttpBody(
    _In_reads_(HeaderBytes) const UCHAR* Buffer,
    _In_ ULONG HeaderBytes,
    _Out_writes_to_(ContentTypeChars, return) PWCHAR ContentType,
    _In_ USHORT ContentTypeChars,
    _Out_writes_to_(ContentEncodingChars, return) PWCHAR ContentEncoding,
    _In_ USHORT ContentEncodingChars
    )
{
    const UCHAR* valueStart = NULL;
    ULONG valueLength = 0;
    ULONG i;

    if (ContentType == NULL || ContentTypeChars == 0 ||
        ContentEncoding == NULL || ContentEncodingChars == 0) {
        return FALSE;
    }

    ContentType[0] = L'\0';
    ContentEncoding[0] = L'\0';

    if (!FindHeaderValue(Buffer, HeaderBytes, "Content-Type:", &valueStart, &valueLength)) {
        return FALSE;
    }

    if (FindHeaderValue(Buffer, HeaderBytes, "Content-Encoding:", &valueStart, &valueLength)) {
        for (i = 0; i < valueLength && i + 1 < ContentEncodingChars; i++) {
            UCHAR ch = valueStart[i];
            if (ch == ';' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
                break;
            }
            ContentEncoding[i] = (WCHAR)ch;
        }
        ContentEncoding[i] = L'\0';

        if (_wcsicmp(ContentEncoding, L"identity") != 0 &&
            _wcsicmp(ContentEncoding, L"gzip") != 0 &&
            _wcsicmp(ContentEncoding, L"deflate") != 0) {
            return FALSE;
        }

        if (!FindHeaderValue(Buffer, HeaderBytes, "Content-Type:", &valueStart, &valueLength)) {
            return FALSE;
        }
    } else {
        RtlStringCchCopyW(ContentEncoding, ContentEncodingChars, L"identity");
    }

    for (i = 0; i < valueLength && i + 1 < ContentTypeChars; i++) {
        UCHAR ch = valueStart[i];
        if (ch == ';' || ch == '\r' || ch == '\n') {
            break;
        }
        ContentType[i] = (WCHAR)ch;
    }
    ContentType[i] = L'\0';

    return _wcsnicmp(ContentType, L"text/", 5) == 0 ||
        _wcsicmp(ContentType, L"application/json") == 0;
}

static
VOID
CaptureHttpBodyPreview(
    _In_reads_(BytesAvailable) const UCHAR* Buffer,
    _In_ ULONG BytesAvailable,
    _Inout_updates_(PreviewCapacity) UCHAR* Preview,
    _In_ ULONG PreviewCapacity,
    _Inout_ PULONG PreviewBytes
    )
{
    ULONG freeBytes;
    ULONG toCopy;

    if (Preview == NULL || PreviewBytes == NULL || *PreviewBytes >= PreviewCapacity) {
        return;
    }

    freeBytes = PreviewCapacity - *PreviewBytes;
    toCopy = BytesAvailable < freeBytes ? BytesAvailable : freeBytes;
    if (toCopy > 0) {
        RtlCopyMemory(Preview + *PreviewBytes, Buffer, toCopy);
        *PreviewBytes += toCopy;
    }
}

static __declspec(noinline)
BOOLEAN
BuildHttpBodySemantic(
    _In_z_ PCWSTR Prefix,
    _In_z_ PCWSTR ContentType,
    _In_reads_(PreviewBytes) const UCHAR* Preview,
    _In_ ULONG PreviewBytes,
    _Out_ PWSTR* Semantic,
    _Out_ PULONG SemanticLength
    )
{
    SIZE_T offset = 0;
    ULONG i;
    PWSTR out;
    SIZE_T finalLen;

    if (Semantic == NULL || SemanticLength == NULL || ContentType == NULL) {
        return FALSE;
    }

    out = (PWSTR)ExAllocatePoolZero(NonPagedPool, 1024 * sizeof(WCHAR), 'bHsP');
    if (out == NULL) return FALSE;
    AppendWideLiteral(Prefix, out, 1024, &offset);
    AppendWideLiteral(L" CT=", out, 1024, &offset);
    AppendWideLiteral(ContentType, out, 1024, &offset);
    AppendWideLiteral(L" | ", out, 1024, &offset);

    for (i = 0; i < PreviewBytes && offset + 1 < 1024; i++) {
        UCHAR ch = Preview[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            out[offset++] = L' ';
        } else if (ch >= 0x20 && ch <= 0x7E) {
            out[offset++] = (WCHAR)ch;
        } else {
            out[offset++] = L'.';
        }
    }
    out[offset] = L'\0';
    finalLen = wcslen(out);
    *Semantic = out;
    *SemanticLength = (ULONG)(finalLen * sizeof(WCHAR));
    return TRUE;
}

static
BOOLEAN
IsJsonContentType(
    _In_z_ PCWSTR ContentType
    )
{
    return _wcsicmp(ContentType, L"application/json") == 0;
}

static
VOID
SkipJsonWhitespace(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Inout_ PULONG Position
    )
{
    while (*Position < Length) {
        UCHAR ch = Buffer[*Position];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            (*Position)++;
        } else {
            break;
        }
    }
}

static
BOOLEAN
ParseJsonStringToken(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Inout_ PULONG Position,
    _Out_writes_to_(OutChars, return) PWCHAR Out,
    _In_ USHORT OutChars
    )
{
    USHORT outIndex = 0;

    if (*Position >= Length || Buffer[*Position] != '"') {
        return FALSE;
    }
    (*Position)++;

    while (*Position < Length) {
        UCHAR ch = Buffer[*Position];
        if (ch == '"') {
            (*Position)++;
            Out[outIndex] = L'\0';
            return TRUE;
        }
        if (ch == '\\') {
            (*Position)++;
            if (*Position >= Length) return FALSE;
            ch = Buffer[*Position];
        }
        if (outIndex + 1 < OutChars) {
            Out[outIndex++] = (ch >= 0x20 && ch <= 0x7E) ? (WCHAR)ch : L'.';
        }
        (*Position)++;
    }

    return FALSE;
}

static
BOOLEAN
ParseJsonPrimitiveToken(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Inout_ PULONG Position,
    _Out_writes_to_(OutChars, return) PWCHAR Out,
    _In_ USHORT OutChars
    )
{
    USHORT outIndex = 0;

    while (*Position < Length) {
        UCHAR ch = Buffer[*Position];
        if (ch == ',' || ch == '}' || ch == ']' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            break;
        }
        if (outIndex + 1 < OutChars) {
            Out[outIndex++] = (ch >= 0x20 && ch <= 0x7E) ? (WCHAR)ch : L'.';
        }
        (*Position)++;
    }

    Out[outIndex] = L'\0';
    return outIndex > 0;
}

static
VOID
AppendJsonPathSegment(
    _In_z_ PCWSTR Base,
    _In_z_ PCWSTR Segment,
    _Out_writes_(PS_JSON_PATH_CHARS) PWCHAR Out
    )
{
    if (Base[0] == L'\0') {
        RtlStringCchCopyW(Out, PS_JSON_PATH_CHARS, Segment);
    } else {
        RtlStringCchPrintfW(Out, PS_JSON_PATH_CHARS, L"%s.%s", Base, Segment);
    }
}

static
VOID
AppendJsonArrayPath(
    _In_z_ PCWSTR Base,
    _In_ ULONG Index,
    _Out_writes_(PS_JSON_PATH_CHARS) PWCHAR Out
    )
{
    if (Base[0] == L'\0') {
        RtlStringCchPrintfW(Out, PS_JSON_PATH_CHARS, L"[%lu]", Index);
    } else {
        RtlStringCchPrintfW(Out, PS_JSON_PATH_CHARS, L"%s[%lu]", Base, Index);
    }
}

static
VOID
AppendJsonSemanticEntry(
    _In_z_ PCWSTR Key,
    _In_z_ PCWSTR Path,
    _In_z_ PCWSTR Value,
    _Inout_updates_(CapacityChars) PWCHAR Out,
    _In_ SIZE_T CapacityChars,
    _Inout_ SIZE_T* Offset
    )
{
    AppendWideLiteral(L" | JSONKEY=", Out, CapacityChars, Offset);
    AppendWideLiteral(Key, Out, CapacityChars, Offset);
    AppendWideLiteral(L" JSONPATH=", Out, CapacityChars, Offset);
    AppendWideLiteral(Path, Out, CapacityChars, Offset);
    AppendWideLiteral(L" JSONVAL=", Out, CapacityChars, Offset);
    AppendWideLiteral(Value, Out, CapacityChars, Offset);
}

static
BOOLEAN
ParseJsonValueSemantic(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Inout_ PULONG Position,
    _In_z_ PCWSTR CurrentPath,
    _In_z_ PCWSTR CurrentKey,
    _Inout_updates_(CapacityChars) PWCHAR Out,
    _In_ SIZE_T CapacityChars,
    _Inout_ SIZE_T* Offset,
    _In_ ULONG Depth,
    _Inout_ PJSON_SEMANTIC_PARSE_SCRATCH Scratch
    );

static
BOOLEAN
ParseJsonObjectSemantic(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Inout_ PULONG Position,
    _In_z_ PCWSTR BasePath,
    _Inout_updates_(CapacityChars) PWCHAR Out,
    _In_ SIZE_T CapacityChars,
    _Inout_ SIZE_T* Offset,
    _In_ ULONG Depth,
    _Inout_ PJSON_SEMANTIC_PARSE_SCRATCH Scratch
    )
{
    if (Depth > PS_JSON_SEMANTIC_MAX_DEPTH ||
        *Position >= Length || Buffer[*Position] != '{') {
        return FALSE;
    }
    (*Position)++;
    SkipJsonWhitespace(Buffer, Length, Position);

    if (*Position < Length && Buffer[*Position] == '}') {
        (*Position)++;
        return TRUE;
    }

    while (*Position < Length) {
        PWCHAR key = Scratch->Keys[Depth];
        PWCHAR childPath = Scratch->Paths[Depth];

        if (!ParseJsonStringToken(Buffer, Length, Position, key, ARRAYSIZE(key))) {
            return FALSE;
        }
        SkipJsonWhitespace(Buffer, Length, Position);
        if (*Position >= Length || Buffer[*Position] != ':') {
            return FALSE;
        }
        (*Position)++;
        SkipJsonWhitespace(Buffer, Length, Position);
        AppendJsonPathSegment(BasePath, key, childPath);
        if (!ParseJsonValueSemantic(
                Buffer, Length, Position, childPath, key, Out, CapacityChars,
                Offset, Depth + 1, Scratch)) {
            return FALSE;
        }
        SkipJsonWhitespace(Buffer, Length, Position);
        if (*Position < Length && Buffer[*Position] == ',') {
            (*Position)++;
            SkipJsonWhitespace(Buffer, Length, Position);
            continue;
        }
        if (*Position < Length && Buffer[*Position] == '}') {
            (*Position)++;
            return TRUE;
        }
        return FALSE;
    }

    return FALSE;
}

static
BOOLEAN
ParseJsonArraySemantic(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Inout_ PULONG Position,
    _In_z_ PCWSTR BasePath,
    _Inout_updates_(CapacityChars) PWCHAR Out,
    _In_ SIZE_T CapacityChars,
    _Inout_ SIZE_T* Offset,
    _In_ ULONG Depth,
    _Inout_ PJSON_SEMANTIC_PARSE_SCRATCH Scratch
    )
{
    ULONG index = 0;

    if (Depth > PS_JSON_SEMANTIC_MAX_DEPTH ||
        *Position >= Length || Buffer[*Position] != '[') {
        return FALSE;
    }
    (*Position)++;
    SkipJsonWhitespace(Buffer, Length, Position);

    if (*Position < Length && Buffer[*Position] == ']') {
        (*Position)++;
        return TRUE;
    }

    while (*Position < Length) {
        PWCHAR itemPath = Scratch->Paths[Depth];
        AppendJsonArrayPath(BasePath, index, itemPath);
        if (!ParseJsonValueSemantic(
                Buffer, Length, Position, itemPath, L"$item", Out, CapacityChars,
                Offset, Depth + 1, Scratch)) {
            return FALSE;
        }
        index++;
        SkipJsonWhitespace(Buffer, Length, Position);
        if (*Position < Length && Buffer[*Position] == ',') {
            (*Position)++;
            SkipJsonWhitespace(Buffer, Length, Position);
            continue;
        }
        if (*Position < Length && Buffer[*Position] == ']') {
            (*Position)++;
            return TRUE;
        }
        return FALSE;
    }

    return FALSE;
}

static
BOOLEAN
ParseJsonValueSemantic(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Inout_ PULONG Position,
    _In_z_ PCWSTR CurrentPath,
    _In_z_ PCWSTR CurrentKey,
    _Inout_updates_(CapacityChars) PWCHAR Out,
    _In_ SIZE_T CapacityChars,
    _Inout_ SIZE_T* Offset,
    _In_ ULONG Depth,
    _Inout_ PJSON_SEMANTIC_PARSE_SCRATCH Scratch
    )
{
    PWCHAR value;

    if (Depth > PS_JSON_SEMANTIC_MAX_DEPTH + 1) return FALSE;
    value = Scratch->Values[Depth];

    SkipJsonWhitespace(Buffer, Length, Position);
    if (*Position >= Length) {
        return FALSE;
    }

    if (Buffer[*Position] == '{') {
        return ParseJsonObjectSemantic(
            Buffer, Length, Position, CurrentPath, Out, CapacityChars, Offset, Depth, Scratch);
    }
    if (Buffer[*Position] == '[') {
        return ParseJsonArraySemantic(
            Buffer, Length, Position, CurrentPath, Out, CapacityChars, Offset, Depth, Scratch);
    }
    if (Buffer[*Position] == '"') {
        if (!ParseJsonStringToken(Buffer, Length, Position, value, ARRAYSIZE(value))) {
            return FALSE;
        }
        AppendJsonSemanticEntry(CurrentKey, CurrentPath, value, Out, CapacityChars, Offset);
        return TRUE;
    }

    if (!ParseJsonPrimitiveToken(Buffer, Length, Position, value, ARRAYSIZE(value))) {
        return FALSE;
    }
    AppendJsonSemanticEntry(CurrentKey, CurrentPath, value, Out, CapacityChars, Offset);
    return TRUE;
}

static __declspec(noinline)
BOOLEAN
BuildJsonBodySemantic(
    _In_z_ PCWSTR Prefix,
    _In_reads_(PreviewBytes) const UCHAR* Preview,
    _In_ ULONG PreviewBytes,
    _Out_ PWSTR* Semantic,
    _Out_ PULONG SemanticLength
    )
{
    SIZE_T offset = 0;
    ULONG pos = 0;
    PWSTR out;
    PJSON_SEMANTIC_PARSE_SCRATCH scratch;
    SIZE_T finalLen;

    out = (PWSTR)ExAllocatePoolZero(NonPagedPool, 1024 * sizeof(WCHAR), 'jHsP');
    if (out == NULL) return FALSE;
    scratch = (PJSON_SEMANTIC_PARSE_SCRATCH)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*scratch),
        'pJsP');
    if (scratch == NULL) {
        ExFreePool(out);
        return FALSE;
    }
    AppendWideLiteral(Prefix, out, 1024, &offset);
    AppendWideLiteral(L" ", out, 1024, &offset);
    AppendWideLiteral(L"JSON", out, 1024, &offset);

    SkipJsonWhitespace(Preview, PreviewBytes, &pos);
    if (pos >= PreviewBytes) {
        ExFreePool(scratch);
        ExFreePool(out);
        return FALSE;
    }

    if (Preview[pos] == '{') {
        if (!ParseJsonObjectSemantic(
                Preview, PreviewBytes, &pos, L"", out, 1024, &offset, 0, scratch)) {
            ExFreePool(scratch);
            ExFreePool(out);
            return FALSE;
        }
    } else if (Preview[pos] == '[') {
        if (!ParseJsonArraySemantic(
                Preview, PreviewBytes, &pos, L"", out, 1024, &offset, 0, scratch)) {
            ExFreePool(scratch);
            ExFreePool(out);
            return FALSE;
        }
    } else {
        ExFreePool(scratch);
        ExFreePool(out);
        return FALSE;
    }

    finalLen = wcslen(out);
    ExFreePool(scratch);
    *Semantic = out;
    *SemanticLength = (ULONG)(finalLen * sizeof(WCHAR));
    return TRUE;
}

static
VOID
ConsumePrefix(
    _Inout_updates_(BufferCapacity) UCHAR* Buffer,
    _Inout_ PULONG BufferLength,
    _In_ ULONG BytesToConsume
    )
{
    if (BytesToConsume >= *BufferLength) {
        *BufferLength = 0;
        return;
    }

    RtlMoveMemory(Buffer, Buffer + BytesToConsume, *BufferLength - BytesToConsume);
    *BufferLength -= BytesToConsume;
}

static
BOOLEAN
ProcessHttpStreamState(
    _In_ UINT64 FlowHandle,
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6,
    _In_ BOOLEAN IsRequest
    )
{
    ULONG offset = 0;
    BOOLEAN shouldBlock = FALSE;
    PUCHAR headerCopy;

    if (FlowHandle == 0) {
        return IsRequest
            ? TryEnqueueHttpRequestEvent(Buffer, BytesCopied, FlowHandle, ProcessId, RemotePort, LocalPort, InFixedValues, InMetaValues, IsV6)
            : TryEnqueueHttpResponseEvent(Buffer, BytesCopied, FlowHandle, ProcessId, RemotePort, LocalPort, InFixedValues, InMetaValues, IsV6);
    }

    headerCopy = (PUCHAR)ExAllocatePoolZero(
        NonPagedPool,
        PS_HTTP_REASSEMBLY_BYTES,
        'bHtP');
    if (headerCopy == NULL) {
        return FALSE;
    }

    while (TRUE) {
        KIRQL oldIrql;
        PSTREAM_FLOW_CACHE_ENTRY entry;
        UCHAR* stateBuffer;
        ULONG* stateBuffered;
        ULONG* stateBodyRemaining;
        BOOLEAN* stateChunked;
        ULONG* stateChunkRemaining;
        BOOLEAN* stateChunkNeedCrlf;
        BOOLEAN* stateChunkTrailer;
        BOOLEAN* stateCaptureBody;
        WCHAR* stateContentType;
        WCHAR* stateContentEncoding;
        UCHAR* stateBodyPreview;
        ULONG* stateBodyPreviewBytes;
        ULONG headerBytes = 0;
        ULONG contentLength = 0;
        BOOLEAN hasMessage = FALSE;
        BOOLEAN madeProgress = FALSE;

        EnsureCachesInitialized();
        KeAcquireSpinLock(&gStreamFlowCacheLock, &oldIrql);
        entry = GetOrCreateFlowEntryLocked(FlowHandle);
        stateBuffer = IsRequest ? entry->HttpReqBuffer : entry->HttpRespBuffer;
        stateBuffered = IsRequest ? &entry->HttpReqBufferedBytes : &entry->HttpRespBufferedBytes;
        stateBodyRemaining = IsRequest ? &entry->HttpReqBodyRemaining : &entry->HttpRespBodyRemaining;
        stateChunked = IsRequest ? &entry->HttpReqChunked : &entry->HttpRespChunked;
        stateChunkRemaining = IsRequest ? &entry->HttpReqChunkRemaining : &entry->HttpRespChunkRemaining;
        stateChunkNeedCrlf = IsRequest ? &entry->HttpReqChunkNeedCrlf : &entry->HttpRespChunkNeedCrlf;
        stateChunkTrailer = IsRequest ? &entry->HttpReqChunkTrailer : &entry->HttpRespChunkTrailer;
        stateCaptureBody = IsRequest ? &entry->HttpReqCaptureBody : &entry->HttpRespCaptureBody;
        stateContentType = IsRequest ? entry->HttpReqContentType : entry->HttpRespContentType;
        stateContentEncoding = IsRequest ? entry->HttpReqContentEncoding : entry->HttpRespContentEncoding;
        stateBodyPreview = IsRequest ? entry->HttpReqBodyPreview : entry->HttpRespBodyPreview;
        stateBodyPreviewBytes = IsRequest ? &entry->HttpReqBodyPreviewBytes : &entry->HttpRespBodyPreviewBytes;

        if (offset < BytesCopied) {
            ULONG toCopy = BytesCopied - offset;
            ULONG freeBytes = PS_HTTP_REASSEMBLY_BYTES - *stateBuffered;
            if (toCopy > freeBytes) {
                toCopy = freeBytes;
            }
            if (toCopy == 0) {
                *stateBuffered = 0;
                *stateBodyRemaining = 0;
                *stateChunked = FALSE;
                *stateChunkRemaining = 0;
                *stateChunkNeedCrlf = FALSE;
                *stateChunkTrailer = FALSE;
                *stateCaptureBody = FALSE;
                *stateBodyPreviewBytes = 0;
                stateContentType[0] = L'\0';
                stateContentEncoding[0] = L'\0';
            } else {
                RtlCopyMemory(stateBuffer + *stateBuffered, Buffer + offset, toCopy);
                *stateBuffered += toCopy;
                offset += toCopy;
                madeProgress = TRUE;
            }
        }

        if (*stateBodyRemaining > 0) {
            if (*stateCaptureBody && *stateBuffered > 0) {
                ULONG bodyBytesNow = *stateBuffered < *stateBodyRemaining ? *stateBuffered : *stateBodyRemaining;
                CaptureHttpBodyPreview(
                    stateBuffer,
                    bodyBytesNow,
                    stateBodyPreview,
                    PS_HTTP_BODY_PREVIEW_BYTES,
                    stateBodyPreviewBytes);
            }
            if (*stateBuffered >= *stateBodyRemaining) {
                ConsumePrefix(stateBuffer, stateBuffered, *stateBodyRemaining);
                *stateBodyRemaining = 0;
                madeProgress = TRUE;
            } else {
                *stateBodyRemaining -= *stateBuffered;
                *stateBuffered = 0;
            }
        }

        if (*stateBodyRemaining == 0 && *stateChunked) {
            if (*stateChunkTrailer) {
                ULONG trailerBytes = 0;
                if (FindHttpHeaderEnd(stateBuffer, *stateBuffered, &trailerBytes)) {
                    PWSTR trailerSemantic = NULL;
                    ULONG trailerSemanticLength = 0;
                    PDLP_NET_EVENT trailerEvent;
                    ULONG trailerChars;

                    if (trailerBytes <= PS_HTTP_REASSEMBLY_BYTES) {
                        RtlCopyMemory(headerCopy, stateBuffer, trailerBytes);
                    }
                    ConsumePrefix(stateBuffer, stateBuffered, trailerBytes);
                    *stateChunked = FALSE;
                    *stateChunkTrailer = FALSE;
                    *stateChunkRemaining = 0;
                    *stateChunkNeedCrlf = FALSE;
                    madeProgress = TRUE;

                    if (!IsRequest && trailerBytes > 0 &&
                        trailerBytes <= PS_HTTP_REASSEMBLY_BYTES &&
                        BuildHttpTrailerSemantic(headerCopy, trailerBytes, &trailerSemantic, &trailerSemanticLength)) {
                        if (!ShouldSuppressDuplicate(FlowHandle, StreamSignalHttpResponse, HashWideString(trailerSemantic))) {
                            trailerEvent = NetEventAllocate();
                            if (trailerEvent != NULL) {
                                if (FinalizeInspectionEvent(
                                        trailerEvent,
                                        ProcessId,
                                        RemotePort,
                                        LocalPort,
                                        EventHttpResponse,
                                        trailerSemantic,
                                        InMetaValues) == ActionBlocked) {
                                    shouldBlock = TRUE;
                                }

                                if (IsV6) CopyAddressesV6(InFixedValues, trailerEvent);
                                else CopyAddressesV4(InFixedValues, trailerEvent);

                                trailerChars = trailerSemanticLength / sizeof(WCHAR);
                                if (trailerChars > ARRAYSIZE(trailerEvent->Url) - 1) trailerChars = ARRAYSIZE(trailerEvent->Url) - 1;
                                trailerEvent->UrlLength = (USHORT)trailerChars;
                                RtlCopyMemory(trailerEvent->Url, trailerSemantic, trailerChars * sizeof(WCHAR));
                                trailerEvent->Url[trailerChars] = L'\0';
                                NetEventEnqueue(trailerEvent);
                                NetEventFree(trailerEvent);
                            }
                        }
                        ExFreePool(trailerSemantic);
                    }
                }
            } else if (*stateChunkNeedCrlf) {
                if (*stateBuffered >= 2) {
                    if (stateBuffer[0] == '\r' && stateBuffer[1] == '\n') {
                        ConsumePrefix(stateBuffer, stateBuffered, 2);
                        *stateChunkNeedCrlf = FALSE;
                        madeProgress = TRUE;
                    } else {
                        *stateBuffered = 0;
                        *stateChunked = FALSE;
                        *stateChunkRemaining = 0;
                        *stateChunkNeedCrlf = FALSE;
                    }
                }
            } else if (*stateChunkRemaining > 0) {
                if (*stateCaptureBody && *stateBuffered > 0) {
                    ULONG chunkBytesNow = *stateBuffered < *stateChunkRemaining ? *stateBuffered : *stateChunkRemaining;
                    CaptureHttpBodyPreview(
                        stateBuffer,
                        chunkBytesNow,
                        stateBodyPreview,
                        PS_HTTP_BODY_PREVIEW_BYTES,
                        stateBodyPreviewBytes);
                }
                if (*stateBuffered >= *stateChunkRemaining) {
                    ConsumePrefix(stateBuffer, stateBuffered, *stateChunkRemaining);
                    *stateChunkRemaining = 0;
                    *stateChunkNeedCrlf = TRUE;
                    madeProgress = TRUE;
                } else {
                    *stateChunkRemaining -= *stateBuffered;
                    *stateBuffered = 0;
                }
            } else {
                ULONG lineBytes = 0;
                ULONG chunkSize = 0;
                if (FindCrlfLineEnd(stateBuffer, *stateBuffered, &lineBytes)) {
                    if (ParseHttpChunkSizeLine(stateBuffer, lineBytes, &chunkSize)) {
                        ConsumePrefix(stateBuffer, stateBuffered, lineBytes);
                        if (chunkSize == 0) {
                            *stateChunkTrailer = TRUE;
                        } else {
                            *stateChunkRemaining = chunkSize;
                        }
                        madeProgress = TRUE;
                    } else {
                        *stateBuffered = 0;
                        *stateChunked = FALSE;
                        *stateChunkRemaining = 0;
                        *stateChunkNeedCrlf = FALSE;
                        *stateChunkTrailer = FALSE;
                    }
                }
            }
        }

        if (*stateBodyRemaining == 0 && !*stateChunked &&
            FindHttpHeaderEnd(stateBuffer, *stateBuffered, &headerBytes) &&
            headerBytes <= PS_HTTP_REASSEMBLY_BYTES) {
            RtlCopyMemory(headerCopy, stateBuffer, headerBytes);
            ConsumePrefix(stateBuffer, stateBuffered, headerBytes);
            if (ParseHttpContentLength(headerCopy, headerBytes, &contentLength)) {
                *stateBodyRemaining = contentLength;
            } else if (ParseHttpChunked(headerCopy, headerBytes)) {
                *stateChunked = TRUE;
                *stateChunkRemaining = 0;
                *stateChunkNeedCrlf = FALSE;
                *stateChunkTrailer = FALSE;
            } else {
                *stateBodyRemaining = 0;
                *stateChunked = FALSE;
                *stateChunkRemaining = 0;
                *stateChunkNeedCrlf = FALSE;
                *stateChunkTrailer = FALSE;
            }
            *stateCaptureBody = ShouldCaptureHttpBody(
                headerCopy,
                headerBytes,
                stateContentType,
                PS_HTTP_CONTENT_TYPE_CHARS,
                stateContentEncoding,
                32);
            *stateBodyPreviewBytes = 0;
            hasMessage = TRUE;
            madeProgress = TRUE;
        }

        KeReleaseSpinLock(&gStreamFlowCacheLock, oldIrql);

        if (hasMessage) {
            if (IsRequest) {
                shouldBlock |= TryEnqueueHttpRequestEvent(
                    headerCopy, headerBytes, FlowHandle, ProcessId, RemotePort, LocalPort, InFixedValues, InMetaValues, IsV6);
            } else {
                shouldBlock |= TryEnqueueHttpResponseEvent(
                    headerCopy, headerBytes, FlowHandle, ProcessId, RemotePort, LocalPort, InFixedValues, InMetaValues, IsV6);
            }
            continue;
        }

        if (*stateBodyRemaining == 0 && !*stateChunked && *stateCaptureBody && *stateBodyPreviewBytes > 0) {
            PWSTR bodySemantic = NULL;
            ULONG bodySemanticLength = 0;
            PDLP_NET_EVENT bodyEvent;
            ULONG bodyChars;

            if ((IsJsonContentType(stateContentType)
                    ? BuildJsonBodySemantic(
                        IsRequest ? L"BODY REQ" : L"BODY RESP",
                        stateBodyPreview,
                        *stateBodyPreviewBytes,
                        &bodySemantic,
                        &bodySemanticLength)
                    : BuildHttpBodySemantic(
                        IsRequest ? L"BODY REQ" : L"BODY RESP",
                        stateContentType,
                        stateBodyPreview,
                        *stateBodyPreviewBytes,
                        &bodySemantic,
                        &bodySemanticLength))) {
                bodyEvent = NetEventAllocate();
                if (bodyEvent != NULL) {
                    if (FinalizeInspectionEvent(
                            bodyEvent,
                            ProcessId,
                            RemotePort,
                            LocalPort,
                            IsRequest ? EventHttpRequest : EventHttpResponse,
                            bodySemantic,
                            InMetaValues) == ActionBlocked) {
                        shouldBlock = TRUE;
                    }

                    if (IsV6) CopyAddressesV6(InFixedValues, bodyEvent);
                    else CopyAddressesV4(InFixedValues, bodyEvent);

                    bodyEvent->ContentTypeLength = (USHORT)wcsnlen(stateContentType, ARRAYSIZE(bodyEvent->ContentType) - 1);
                    RtlCopyMemory(bodyEvent->ContentType, stateContentType, bodyEvent->ContentTypeLength * sizeof(WCHAR));
                    bodyEvent->ContentType[bodyEvent->ContentTypeLength] = L'\0';
                    bodyEvent->ContentEncodingLength = (USHORT)wcsnlen(stateContentEncoding, ARRAYSIZE(bodyEvent->ContentEncoding) - 1);
                    RtlCopyMemory(bodyEvent->ContentEncoding, stateContentEncoding, bodyEvent->ContentEncodingLength * sizeof(WCHAR));
                    bodyEvent->ContentEncoding[bodyEvent->ContentEncodingLength] = L'\0';
                    bodyEvent->BodyPreviewLength = (USHORT)(*stateBodyPreviewBytes > ARRAYSIZE(bodyEvent->BodyPreview)
                        ? ARRAYSIZE(bodyEvent->BodyPreview)
                        : *stateBodyPreviewBytes);
                    if (bodyEvent->BodyPreviewLength > 0) {
                        RtlCopyMemory(bodyEvent->BodyPreview, stateBodyPreview, bodyEvent->BodyPreviewLength);
                    }

                    bodyChars = bodySemanticLength / sizeof(WCHAR);
                    if (bodyChars > ARRAYSIZE(bodyEvent->Url) - 1) bodyChars = ARRAYSIZE(bodyEvent->Url) - 1;
                    bodyEvent->UrlLength = (USHORT)bodyChars;
                    RtlCopyMemory(bodyEvent->Url, bodySemantic, bodyChars * sizeof(WCHAR));
                    bodyEvent->Url[bodyChars] = L'\0';
                    NetEventEnqueue(bodyEvent);
                    NetEventFree(bodyEvent);
                }
                ExFreePool(bodySemantic);
            }

            *stateCaptureBody = FALSE;
            *stateBodyPreviewBytes = 0;
            stateContentType[0] = L'\0';
            stateContentEncoding[0] = L'\0';
            if (offset >= BytesCopied && !madeProgress) {
                break;
            }
            continue;
        }

        if (offset >= BytesCopied && !madeProgress) {
            break;
        }
        if (offset >= BytesCopied && !hasMessage && !madeProgress) {
            break;
        }
        if (offset >= BytesCopied && !hasMessage && madeProgress) {
            continue;
        }
    }

    ExFreePool(headerCopy);
    return shouldBlock;
}

static
FWP_DIRECTION
GetPacketDirection(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ BOOLEAN IsV6
    )
{
    if (InFixedValues == NULL) {
        return FWP_DIRECTION_MAX;
    }

    return (FWP_DIRECTION)(
        IsV6
            ? InFixedValues->incomingValue[FWPS_FIELD_STREAM_V6_DIRECTION].value.uint32
            : InFixedValues->incomingValue[FWPS_FIELD_STREAM_V4_DIRECTION].value.uint32);
}

static
VOID
PrepareStreamDefaults(
    _Inout_ FWPS_STREAM_CALLOUT_IO_PACKET0* IoPacket,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    IoPacket->countBytesRequired = 0;
    IoPacket->countBytesEnforced = 0;
    IoPacket->streamAction = FWPS_STREAM_ACTION_NONE;
    if ((ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) != 0) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
    }
}

static
VOID
CopyAddressesV4(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _Inout_ PDLP_NET_EVENT Event
    )
{
    Event->LocalAddressLength = FormatIpv4(
        InFixedValues->incomingValue[FWPS_FIELD_STREAM_V4_IP_LOCAL_ADDRESS].value.uint32,
        Event->LocalAddress,
        sizeof(Event->LocalAddress));
    Event->RemoteAddressLength = FormatIpv4(
        InFixedValues->incomingValue[FWPS_FIELD_STREAM_V4_IP_REMOTE_ADDRESS].value.uint32,
        Event->RemoteAddress,
        sizeof(Event->RemoteAddress));
}

static
VOID
CopyAddressesV6(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _Inout_ PDLP_NET_EVENT Event
    )
{
    Event->LocalAddressLength = FormatIpv6(
        InFixedValues->incomingValue[FWPS_FIELD_STREAM_V6_IP_LOCAL_ADDRESS].value.byteArray16,
        Event->LocalAddress,
        sizeof(Event->LocalAddress));
    Event->RemoteAddressLength = FormatIpv6(
        InFixedValues->incomingValue[FWPS_FIELD_STREAM_V6_IP_REMOTE_ADDRESS].value.byteArray16,
        Event->RemoteAddress,
        sizeof(Event->RemoteAddress));
}

static
VOID
BuildAddressBytesFromStream(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ BOOLEAN IsV6,
    _Out_writes_(16) UCHAR* LocalAddr,
    _Out_writes_(16) UCHAR* RemoteAddr
    )
{
    if (IsV6) {
        CopyAddressBytesV6(
            InFixedValues->incomingValue[FWPS_FIELD_STREAM_V6_IP_LOCAL_ADDRESS].value.byteArray16,
            LocalAddr);
        CopyAddressBytesV6(
            InFixedValues->incomingValue[FWPS_FIELD_STREAM_V6_IP_REMOTE_ADDRESS].value.byteArray16,
            RemoteAddr);
    } else {
        RtlZeroMemory(LocalAddr, 16);
        RtlZeroMemory(RemoteAddr, 16);
        CopyAddressBytesV4(
            InFixedValues->incomingValue[FWPS_FIELD_STREAM_V4_IP_LOCAL_ADDRESS].value.uint32,
            LocalAddr);
        CopyAddressBytesV4(
            InFixedValues->incomingValue[FWPS_FIELD_STREAM_V4_IP_REMOTE_ADDRESS].value.uint32,
            RemoteAddr);
    }
}

static
BOOLEAN
TryEnqueueHttpRequestEvent(
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    )
{
    BOOLEAN shouldBlock = FALSE;
    ULONG cursor = 0;

    while (cursor < BytesCopied) {
        PWSTR parsedUrl = NULL;
        ULONG parsedUrlLength = 0;
        PWSTR semantic = NULL;
        ULONG semanticLength = 0;
        ULONG headerBytes = 0;
        PDLP_NET_EVENT event;
        ULONG urlChars;
        const UCHAR* slice = Buffer + cursor;
        ULONG remaining = BytesCopied - cursor;

        if (!FindHttpHeaderEnd(slice, remaining, &headerBytes)) {
            break;
        }
        if (!NT_SUCCESS(ParseHttpTraffic(slice, headerBytes, &parsedUrl, &parsedUrlLength))) {
            cursor += headerBytes;
            continue;
        }

        if (!ShouldSuppressDuplicate(FlowHandle, StreamSignalHttpRequest, HashWideString(parsedUrl))) {
            event = NetEventAllocate();
            if (event != NULL) {
                if (!BuildHttpHeaderSemantic(slice, headerBytes, L"REQ", parsedUrl, TRUE, &semantic, &semanticLength)) {
                    semantic = parsedUrl;
                    semanticLength = parsedUrlLength;
                }
                if (FinalizeInspectionEvent(
                        event,
                        ProcessId,
                        RemotePort,
                        LocalPort,
                        EventHttpRequest,
                        semantic,
                        InMetaValues) == ActionBlocked) {
                    shouldBlock = TRUE;
                }

                if (IsV6) CopyAddressesV6(InFixedValues, event);
                else CopyAddressesV4(InFixedValues, event);

                urlChars = semanticLength / sizeof(WCHAR);
                if (urlChars > ARRAYSIZE(event->Url) - 1) urlChars = ARRAYSIZE(event->Url) - 1;
                event->UrlLength = (USHORT)urlChars;
                RtlCopyMemory(event->Url, semantic, urlChars * sizeof(WCHAR));
                event->Url[urlChars] = L'\0';
                NetEventEnqueue(event);
                NetEventFree(event);

                if (semantic != NULL && semantic != parsedUrl) {
                    ExFreePool(semantic);
                }
            }
        }

        ExFreePool(parsedUrl);
        cursor += headerBytes;
    }
    return shouldBlock;
}

static
BOOLEAN
TakeFtpDirectoryLine(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_reads_(BufferLength) const UCHAR* Buffer,
    _In_ ULONG BufferLength,
    _Inout_ PULONG Cursor,
    _Out_writes_to_(LineCapacity, *LineLength) PUCHAR Line,
    _In_ ULONG LineCapacity,
    _Out_ PULONG LineLength,
    _Out_ PBOOLEAN Truncated
    )
{
    KIRQL oldIrql;
    PFTP_ACTIVE_FLOW match = NULL;
    ULONG i;

    *LineLength = 0;
    *Truncated = FALSE;
    EnsureCachesInitialized();
    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW entry = &gFtpActiveFlows[i];
        if (!entry->InUse || entry->ProcessId != ProcessId || entry->IsV6 != IsV6 ||
            entry->LocalPort != LocalPort || entry->RemotePort != RemotePort) {
            continue;
        }
        if (AddressEquals(entry->LocalAddress, LocalAddr, IsV6 ? 16u : 4u) &&
            AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            match = entry;
            break;
        }
    }

    if (match == NULL) {
        *Cursor = BufferLength;
        KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
        return FALSE;
    }

    while (*Cursor < BufferLength) {
        UCHAR ch = Buffer[(*Cursor)++];
        if (ch == '\r' || ch == '\n') {
            if (match->DirectoryLineBytes == 0 && !match->DirectoryLineOverflow) {
                continue;
            }
            *LineLength = min((ULONG)match->DirectoryLineBytes, LineCapacity);
            if (*LineLength > 0) {
                RtlCopyMemory(Line, match->DirectoryLineBuffer, *LineLength);
            }
            *Truncated = match->DirectoryLineOverflow || match->DirectoryLineBytes > LineCapacity;
            match->DirectoryLineBytes = 0;
            match->DirectoryLineOverflow = FALSE;
            KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
            return TRUE;
        }

        if (match->DirectoryLineBytes < ARRAYSIZE(match->DirectoryLineBuffer)) {
            match->DirectoryLineBuffer[match->DirectoryLineBytes++] = ch;
        } else {
            match->DirectoryLineOverflow = TRUE;
        }
    }

    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
    return FALSE;
}

static
VOID
UpdateFtpDirectoryEntryStats(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_ const FTP_DIRECTORY_ENTRY* Entry,
    _In_ BOOLEAN Truncated,
    _In_ BOOLEAN EventSuppressed
    )
{
    KIRQL oldIrql;
    ULONG i;

    EnsureCachesInitialized();
    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW active = &gFtpActiveFlows[i];
        if (!active->InUse || active->ProcessId != ProcessId || active->IsV6 != IsV6 ||
            active->LocalPort != LocalPort || active->RemotePort != RemotePort ||
            !AddressEquals(active->LocalAddress, LocalAddr, IsV6 ? 16u : 4u) ||
            !AddressEquals(active->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            continue;
        }

        active->DirectoryEntryCount++;
        if (Entry->Type == FtpDirectoryEntryFile) {
            active->DirectoryFileCount++;
            if (Entry->HasSize) active->ListedFileBytes += Entry->Size;
        } else if (Entry->Type == FtpDirectoryEntryDirectory) {
            active->DirectoryDirectoryCount++;
        } else if (Entry->Type == FtpDirectoryEntryLink) {
            active->DirectoryLinkCount++;
        }
        if (Truncated) active->DirectoryHadTruncatedLine = TRUE;
        if (EventSuppressed) active->DirectoryEntryEventsSuppressed++;
        break;
    }
    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
}

static
PCWSTR
FtpDirectoryEntryTypeName(
    _In_ FTP_DIRECTORY_ENTRY_TYPE Type
    )
{
    switch (Type) {
        case FtpDirectoryEntryFile: return L"file";
        case FtpDirectoryEntryDirectory: return L"directory";
        case FtpDirectoryEntryLink: return L"link";
        default: return L"unknown";
    }
}

static
VOID
SanitizeFtpDirectorySemanticValue(
    _Inout_updates_z_(CapacityChars) PWCHAR Value,
    _In_ SIZE_T CapacityChars
    )
{
    SIZE_T i;

    UNREFERENCED_PARAMETER(CapacityChars);
    for (i = 0; Value[i] != L'\0'; i++) {
        if (Value[i] == L'"') Value[i] = L'\'';
        else if (Value[i] == L'\r' || Value[i] == L'\n' || Value[i] == L'\t') Value[i] = L' ';
    }
}

static
BOOLEAN
EnqueueFtpDirectoryEntryEvent(
    _In_ const FTP_DIRECTORY_ENTRY* ParsedEntry,
    _In_z_ PCWSTR Command,
    _In_ BOOLEAN Truncated,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    )
{
    PFTP_DIRECTORY_EVENT_SCRATCH scratch;
    PDLP_NET_EVENT event;
    SIZE_T semanticLength;
    BOOLEAN shouldBlock = FALSE;

    scratch = (PFTP_DIRECTORY_EVENT_SCRATCH)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*scratch),
        'eFsP');
    if (scratch == NULL) {
        return FALSE;
    }

    RtlCopyMemory(&scratch->Entry, ParsedEntry, sizeof(scratch->Entry));
    SanitizeFtpDirectorySemanticValue(scratch->Entry.Name, ARRAYSIZE(scratch->Entry.Name));
    SanitizeFtpDirectorySemanticValue(scratch->Entry.LinkTarget, ARRAYSIZE(scratch->Entry.LinkTarget));
    SanitizeFtpDirectorySemanticValue(scratch->Entry.Owner, ARRAYSIZE(scratch->Entry.Owner));
    SanitizeFtpDirectorySemanticValue(scratch->Entry.Group, ARRAYSIZE(scratch->Entry.Group));
    if (scratch->Entry.HasSize) RtlStringCchPrintfW(scratch->SizeText, ARRAYSIZE(scratch->SizeText), L"%I64u", scratch->Entry.Size);
    else RtlStringCchCopyW(scratch->SizeText, ARRAYSIZE(scratch->SizeText), L"-");
    if (scratch->Entry.HasAllocationBlocks) RtlStringCchPrintfW(scratch->BlockText, ARRAYSIZE(scratch->BlockText), L"%I64u", scratch->Entry.AllocationBlocks);
    else RtlStringCchCopyW(scratch->BlockText, ARRAYSIZE(scratch->BlockText), L"-");

    RtlStringCchPrintfW(
        scratch->Semantic,
        ARRAYSIZE(scratch->Semantic),
        L"FTP DIR_ENTRY command=\"%s\" format=%s type=%s name=\"%s\" size=%s blocks=%s modified=\"%s\" owner=\"%s\" group=\"%s\" perms=\"%s\" target=\"%s\" truncated=%u",
        Command,
        scratch->Entry.Format,
        FtpDirectoryEntryTypeName(scratch->Entry.Type),
        scratch->Entry.Name,
        scratch->SizeText,
        scratch->BlockText,
        scratch->Entry.Modified,
        scratch->Entry.Owner,
        scratch->Entry.Group,
        scratch->Entry.Permissions,
        scratch->Entry.LinkTarget,
        Truncated ? 1u : 0u);

    event = NetEventAllocate();
    if (event == NULL) {
        ExFreePool(scratch);
        return FALSE;
    }
    if (FinalizeInspectionEvent(
            event, ProcessId, RemotePort, LocalPort, EventFtpCommand, scratch->Semantic, InMetaValues) == ActionBlocked) {
        shouldBlock = TRUE;
    }
    if (IsV6) CopyAddressesV6(InFixedValues, event);
    else CopyAddressesV4(InFixedValues, event);

    semanticLength = wcsnlen(scratch->Semantic, ARRAYSIZE(event->Url) - 1);
    event->UrlLength = (USHORT)semanticLength;
    RtlCopyMemory(event->Url, scratch->Semantic, semanticLength * sizeof(WCHAR));
    event->Url[semanticLength] = L'\0';
    NetEventEnqueue(event);
    NetEventFree(event);
    ExFreePool(scratch);
    return shouldBlock;
}

static
PCWSTR
DetectFtpContentType(
    _In_reads_(Length) const UCHAR* Buffer,
    _In_ ULONG Length
    )
{
    ULONG i;
    ULONG printable = 0;
    ULONG sample = min(Length, 256u);

    if (Length >= 5 && RtlCompareMemory(Buffer, "%PDF-", 5) == 5) return L"application/pdf";
    if (Length >= 4 && Buffer[0] == 'P' && Buffer[1] == 'K' &&
        ((Buffer[2] == 3 && Buffer[3] == 4) || (Buffer[2] == 5 && Buffer[3] == 6) ||
         (Buffer[2] == 7 && Buffer[3] == 8))) return L"application/zip";
    if (Length >= 8 && Buffer[0] == 0x89 && RtlCompareMemory(Buffer + 1, "PNG\r\n\x1a\n", 7) == 7) return L"image/png";
    if (Length >= 3 && Buffer[0] == 0xFF && Buffer[1] == 0xD8 && Buffer[2] == 0xFF) return L"image/jpeg";
    if (Length >= 2 && Buffer[0] == 0x1F && Buffer[1] == 0x8B) return L"application/gzip";
    if (Length >= 2 && Buffer[0] == 'M' && Buffer[1] == 'Z') return L"application/x-msdownload";
    if (Length >= 8 && Buffer[0] == 0xD0 && Buffer[1] == 0xCF && Buffer[2] == 0x11 && Buffer[3] == 0xE0 &&
        Buffer[4] == 0xA1 && Buffer[5] == 0xB1 && Buffer[6] == 0x1A && Buffer[7] == 0xE1) {
        return L"application/x-ole-storage";
    }

    for (i = 0; i < sample; i++) {
        UCHAR ch = Buffer[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (ch == '{' || ch == '[') return L"application/json";
        break;
    }
    for (i = 0; i < sample; i++) {
        UCHAR ch = Buffer[i];
        if ((ch >= 0x20 && ch <= 0x7E) || ch == '\t' || ch == '\r' || ch == '\n' || ch >= 0x80) {
            printable++;
        }
    }
    return sample > 0 && printable * 100 / sample >= 85 ? L"text/plain" : L"application/octet-stream";
}

static
BOOLEAN
BuildFtpDataContentSemantic(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_reads_(BufferLength) const UCHAR* Buffer,
    _In_ ULONG BufferLength,
    _In_z_ PCWSTR Command,
    _In_ BOOLEAN OutboundObserved,
    _Out_writes_(SemanticChars) PWCHAR Semantic,
    _In_ SIZE_T SemanticChars,
    _Out_writes_(ContentTypeChars) PWCHAR ContentType,
    _In_ SIZE_T ContentTypeChars
    )
{
    KIRQL oldIrql;
    PFTP_ACTIVE_FLOW match = NULL;
    PFTP_CONTENT_BUILD_SCRATCH scratch;
    ULONG combinedBytes = 0;
    ULONG copyBytes;
    ULONG keepBytes;
    PCWSTR detected;
    ULONG i;

    if (BufferLength == 0 || SemanticChars == 0 || ContentTypeChars == 0) return FALSE;
    scratch = (PFTP_CONTENT_BUILD_SCRATCH)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*scratch),
        'bFsP');
    if (scratch == NULL) return FALSE;
    detected = DetectFtpContentType(Buffer, BufferLength);
    EnsureCachesInitialized();
    KeAcquireSpinLock(&gFtpActiveFlowLock, &oldIrql);
    for (i = 0; i < PS_FTP_ACTIVE_FLOW_SIZE; i++) {
        PFTP_ACTIVE_FLOW entry = &gFtpActiveFlows[i];
        if (!entry->InUse || entry->ProcessId != ProcessId || entry->IsV6 != IsV6 ||
            entry->LocalPort != LocalPort || entry->RemotePort != RemotePort ||
            !AddressEquals(entry->LocalAddress, LocalAddr, IsV6 ? 16u : 4u) ||
            !AddressEquals(entry->RemoteAddress, RemoteAddr, IsV6 ? 16u : 4u)) {
            continue;
        }
        match = entry;
        break;
    }
    if (match == NULL) {
        KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);
        ExFreePool(scratch);
        return FALSE;
    }

    if (match->ContentTailBytes > 0) {
        combinedBytes = min((ULONG)match->ContentTailBytes, (ULONG)ARRAYSIZE(scratch->Combined));
        RtlCopyMemory(scratch->Combined, match->ContentTail, combinedBytes);
    }
    copyBytes = min(BufferLength, (ULONG)ARRAYSIZE(scratch->Combined) - combinedBytes);
    if (copyBytes > 0) {
        RtlCopyMemory(scratch->Combined + combinedBytes, Buffer, copyBytes);
        combinedBytes += copyBytes;
    }

    if (BufferLength >= PS_FTP_CONTENT_TAIL_BYTES) {
        RtlCopyMemory(
            match->ContentTail,
            Buffer + BufferLength - PS_FTP_CONTENT_TAIL_BYTES,
            PS_FTP_CONTENT_TAIL_BYTES);
        match->ContentTailBytes = PS_FTP_CONTENT_TAIL_BYTES;
    } else {
        keepBytes = min((ULONG)match->ContentTailBytes, PS_FTP_CONTENT_TAIL_BYTES - BufferLength);
        if (keepBytes > 0 && keepBytes < match->ContentTailBytes) {
            RtlMoveMemory(
                match->ContentTail,
                match->ContentTail + match->ContentTailBytes - keepBytes,
                keepBytes);
        }
        RtlCopyMemory(match->ContentTail + keepBytes, Buffer, BufferLength);
        match->ContentTailBytes = (USHORT)(keepBytes + BufferLength);
    }

    if (match->DetectedContentType[0] == L'\0') {
        RtlStringCchCopyW(match->DetectedContentType, ARRAYSIZE(match->DetectedContentType), detected);
    }
    RtlStringCchCopyW(ContentType, ContentTypeChars, match->DetectedContentType);
    KeReleaseSpinLock(&gFtpActiveFlowLock, oldIrql);

    DecodeFtpDirectoryText(
        scratch->Combined,
        combinedBytes,
        scratch->Preview,
        ARRAYSIZE(scratch->Preview));
    SanitizeFtpDirectorySemanticValue(scratch->Preview, ARRAYSIZE(scratch->Preview));
    RtlStringCchPrintfW(
        Semantic,
        SemanticChars,
        L"FTP DATA_CONTENT command=\"%s\" direction=%s mime=\"%s\" preview=\"%s\"",
        Command,
        OutboundObserved ? L"OUT" : L"IN",
        ContentType,
        scratch->Preview);
    ExFreePool(scratch);
    return TRUE;
}

static
BOOLEAN
EnqueueFtpDataContentEvent(
    _In_z_ PCWSTR Semantic,
    _In_z_ PCWSTR ContentType,
    _In_reads_(PreviewLength) const UCHAR* Preview,
    _In_ ULONG PreviewLength,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    )
{
    PDLP_NET_EVENT event;
    SIZE_T semanticLength;
    BOOLEAN shouldBlock = FALSE;

    event = NetEventAllocate();
    if (event == NULL) return FALSE;
    if (FinalizeInspectionEvent(
            event, ProcessId, RemotePort, LocalPort, EventFtpCommand, Semantic, InMetaValues) == ActionBlocked) {
        shouldBlock = TRUE;
    }
    if (IsV6) CopyAddressesV6(InFixedValues, event);
    else CopyAddressesV4(InFixedValues, event);

    event->ContentTypeLength = (USHORT)wcsnlen(ContentType, ARRAYSIZE(event->ContentType) - 1);
    RtlCopyMemory(event->ContentType, ContentType, event->ContentTypeLength * sizeof(WCHAR));
    event->ContentType[event->ContentTypeLength] = L'\0';
    event->BodyPreviewLength = (USHORT)min(PreviewLength, (ULONG)ARRAYSIZE(event->BodyPreview));
    if (event->BodyPreviewLength > 0) {
        RtlCopyMemory(event->BodyPreview, Preview, event->BodyPreviewLength);
    }
    semanticLength = wcsnlen(Semantic, ARRAYSIZE(event->Url) - 1);
    event->UrlLength = (USHORT)semanticLength;
    RtlCopyMemory(event->Url, Semantic, semanticLength * sizeof(WCHAR));
    event->Url[semanticLength] = L'\0';
    NetEventEnqueue(event);
    NetEventFree(event);
    return shouldBlock;
}

static
BOOLEAN
TryEnqueueFtpDataPreviewEvent(
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6,
    _In_ BOOLEAN OutboundObserved
    )
{
    PFTP_DATA_PREVIEW_SCRATCH scratch;
    PWSTR previewLine = NULL;
    ULONG previewLength = 0;
    PDLP_NET_EVENT event;
    size_t len;
    BOOLEAN shouldBlock = FALSE;
    ULONG cursor = 0;
    ULONG emittedEntries = 0;
    ULONG directoryLineLength;
    BOOLEAN directoryLineTruncated;
    ULONG contentCursor = 0;
    BOOLEAN emittedContent = FALSE;

    if (!FindFtpActiveFlow(
            ProcessId,
            IsV6,
            OutboundObserved,
            LocalPort,
            RemotePort,
            LocalAddr,
            RemoteAddr,
            NULL,
            0)) {
        return FALSE;
    }

    scratch = (PFTP_DATA_PREVIEW_SCRATCH)ExAllocateFromNPagedLookasideList(
        &gFtpPreviewScratchLookaside);
    if (scratch == NULL) {
        return FALSE;
    }
    RtlZeroMemory(scratch, sizeof(*scratch));

    if (!FindFtpActiveFlow(
            ProcessId,
            IsV6,
            OutboundObserved,
            LocalPort,
            RemotePort,
            LocalAddr,
            RemoteAddr,
            scratch->Command,
            ARRAYSIZE(scratch->Command))) {
        ExFreeToNPagedLookasideList(&gFtpPreviewScratchLookaside, scratch);
        return FALSE;
    }

    UpdateFtpActiveFlowBytes(
        ProcessId,
        IsV6,
        OutboundObserved,
        LocalPort,
        RemotePort,
        LocalAddr,
        RemoteAddr,
        BytesCopied);

    if (IsFtpDirectoryCommand(scratch->Command)) {
        while (cursor < BytesCopied) {
            BOOLEAN parsed;
            BOOLEAN suppressEvent;

            if (!TakeFtpDirectoryLine(
                    ProcessId,
                    IsV6,
                    LocalPort,
                    RemotePort,
                    LocalAddr,
                    RemoteAddr,
                    Buffer,
                    BytesCopied,
                    &cursor,
                    scratch->DirectoryLine,
                    ARRAYSIZE(scratch->DirectoryLine),
                    &directoryLineLength,
                    &directoryLineTruncated)) {
                break;
            }

            parsed = ParseFtpDirectoryEntry(
                scratch->DirectoryLine,
                directoryLineLength,
                scratch->Command,
                &scratch->DirectoryEntry);
            if (!parsed) {
                continue;
            }

            suppressEvent = emittedEntries >= PS_FTP_DIRECTORY_EVENTS_PER_CALLBACK;
            UpdateFtpDirectoryEntryStats(
                ProcessId,
                IsV6,
                LocalPort,
                RemotePort,
                LocalAddr,
                RemoteAddr,
                &scratch->DirectoryEntry,
                directoryLineTruncated,
                suppressEvent);
            if (!suppressEvent) {
                shouldBlock |= EnqueueFtpDirectoryEntryEvent(
                    &scratch->DirectoryEntry,
                    scratch->Command,
                    directoryLineTruncated,
                    ProcessId,
                    RemotePort,
                    LocalPort,
                    InFixedValues,
                    InMetaValues,
                    IsV6);
                emittedEntries++;
            }
        }
        ExFreeToNPagedLookasideList(&gFtpPreviewScratchLookaside, scratch);
        return shouldBlock;
    }

    while (contentCursor < BytesCopied) {
        ULONG contentBytes = min(PS_FTP_CONTENT_CHUNK_BYTES, BytesCopied - contentCursor);
        scratch->ContentType[0] = L'\0';
        if (!BuildFtpDataContentSemantic(
                ProcessId,
                IsV6,
                LocalPort,
                RemotePort,
                LocalAddr,
                RemoteAddr,
                Buffer + contentCursor,
                contentBytes,
                scratch->Command,
                OutboundObserved,
                scratch->Decorated,
                ARRAYSIZE(scratch->Decorated),
                scratch->ContentType,
                ARRAYSIZE(scratch->ContentType))) {
            break;
        }
        shouldBlock |= EnqueueFtpDataContentEvent(
            scratch->Decorated,
            scratch->ContentType,
            Buffer + contentCursor,
            contentBytes,
            ProcessId,
            RemotePort,
            LocalPort,
            InFixedValues,
            InMetaValues,
            IsV6);
        emittedContent = TRUE;
        contentCursor += contentBytes;
    }
    if (emittedContent) {
        ExFreeToNPagedLookasideList(&gFtpPreviewScratchLookaside, scratch);
        return shouldBlock;
    }

    if (!BuildUnicodePreviewLine(Buffer, BytesCopied, &previewLine, &previewLength)) {
        RtlStringCchPrintfW(
            scratch->Decorated,
            ARRAYSIZE(scratch->Decorated),
            L"DATA %s %s %lu bytes",
            OutboundObserved ? L"OUT" : L"IN",
            scratch->Command[0] != L'\0' ? scratch->Command : L"UNKNOWN",
            BytesCopied);
    } else {
        RtlStringCchPrintfW(
            scratch->Decorated,
            ARRAYSIZE(scratch->Decorated),
            L"DATA PREVIEW %s %s",
            scratch->Command[0] != L'\0' ? scratch->Command : L"UNKNOWN",
            previewLine);
    }

    event = NetEventAllocate();
    if (event == NULL) {
        if (previewLine != NULL) ExFreePool(previewLine);
        ExFreeToNPagedLookasideList(&gFtpPreviewScratchLookaside, scratch);
        return FALSE;
    }
    if (FinalizeInspectionEvent(
            event,
            ProcessId,
            RemotePort,
            LocalPort,
            EventFtpCommand,
            scratch->Decorated,
            InMetaValues) == ActionBlocked) {
        shouldBlock = TRUE;
    }

    if (IsV6) CopyAddressesV6(InFixedValues, event);
    else CopyAddressesV4(InFixedValues, event);

    len = wcsnlen(scratch->Decorated, ARRAYSIZE(event->Url) - 1);
    event->UrlLength = (USHORT)len;
    RtlCopyMemory(event->Url, scratch->Decorated, len * sizeof(WCHAR));
    event->Url[len] = L'\0';
    NetEventEnqueue(event);
    NetEventFree(event);

    if (previewLine != NULL) {
        ExFreePool(previewLine);
    }
    ExFreeToNPagedLookasideList(&gFtpPreviewScratchLookaside, scratch);
    return shouldBlock;
}

static
BOOLEAN
TryEnqueueHttpResponseEvent(
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    )
{
    BOOLEAN shouldBlock = FALSE;
    ULONG cursor = 0;

    while (cursor < BytesCopied) {
        PWSTR statusLine = NULL;
        ULONG statusLineLength = 0;
        PWSTR semantic = NULL;
        ULONG semanticLength = 0;
        ULONG headerBytes = 0;
        PDLP_NET_EVENT event;
        ULONG chars;
        const UCHAR* slice = Buffer + cursor;
        ULONG remaining = BytesCopied - cursor;

        if (!FindHttpHeaderEnd(slice, remaining, &headerBytes)) {
            break;
        }
        if (!StartsWithHttpResponse(slice, headerBytes) ||
            !BuildUnicodeStatusLine(slice, headerBytes, &statusLine, &statusLineLength)) {
            cursor += headerBytes;
            continue;
        }

        if (!ShouldSuppressDuplicate(FlowHandle, StreamSignalHttpResponse, HashWideString(statusLine))) {
            event = NetEventAllocate();
            if (event != NULL) {
                if (!BuildHttpHeaderSemantic(slice, headerBytes, L"RESP", statusLine, FALSE, &semantic, &semanticLength)) {
                    semantic = statusLine;
                    semanticLength = statusLineLength;
                }
                if (FinalizeInspectionEvent(
                        event,
                        ProcessId,
                        RemotePort,
                        LocalPort,
                        EventHttpResponse,
                        semantic,
                        InMetaValues) == ActionBlocked) {
                    shouldBlock = TRUE;
                }

                if (IsV6) CopyAddressesV6(InFixedValues, event);
                else CopyAddressesV4(InFixedValues, event);

                chars = semanticLength / sizeof(WCHAR);
                if (chars > ARRAYSIZE(event->Url) - 1) chars = ARRAYSIZE(event->Url) - 1;
                event->UrlLength = (USHORT)chars;
                RtlCopyMemory(event->Url, semantic, chars * sizeof(WCHAR));
                event->Url[chars] = L'\0';
                NetEventEnqueue(event);
                NetEventFree(event);

                if (semantic != NULL && semantic != statusLine) {
                    ExFreePool(semantic);
                }
            }
        }

        ExFreePool(statusLine);
        cursor += headerBytes;
    }
    return shouldBlock;
}

static
BOOLEAN
TryEnqueueSniEvent(
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    )
{
    PSNI_CAPTURE_RESULT sniResult;
    PDLP_NET_EVENT event;
    size_t sniLen;
    BOOLEAN shouldBlock = FALSE;

    sniResult = (PSNI_CAPTURE_RESULT)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*sniResult),
        'sNsP');
    if (sniResult == NULL) return FALSE;

    if (!NT_SUCCESS(SnipCaptureParseClientHello(Buffer, BytesCopied, sniResult))) {
        ExFreePool(sniResult);
        return FALSE;
    }

    if (!ShouldSuppressDuplicate(FlowHandle, StreamSignalSni, HashWideString((PCWSTR)sniResult->DomainName))) {
        event = NetEventAllocate();
        if (event == NULL) {
            ExFreePool(sniResult);
            return FALSE;
        }
        if (FinalizeInspectionEvent(
                event,
                ProcessId,
                RemotePort,
                LocalPort,
                EventSniCapture,
                (PCWSTR)sniResult->DomainName,
                InMetaValues) == ActionBlocked) {
            shouldBlock = TRUE;
        }

        if (IsV6) CopyAddressesV6(InFixedValues, event);
        else CopyAddressesV4(InFixedValues, event);

        sniLen = wcsnlen((const WCHAR*)sniResult->DomainName, ARRAYSIZE(event->SniDomain) - 1);
        event->SniDomainLength = (USHORT)sniLen;
        RtlCopyMemory(event->SniDomain, sniResult->DomainName, sniLen * sizeof(WCHAR));
        event->SniDomain[sniLen] = L'\0';
        event->Timestamp = sniResult->Timestamp;
        NetEventEnqueue(event);
        NetEventFree(event);
    }

    ExFreePool(sniResult);
    return shouldBlock;
}

static
BOOLEAN
TryEnqueueFtpCommandEvent(
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    )
{
    WCHAR ftpCommand[PS_URL_LEN];
    PDLP_NET_EVENT event;
    size_t commandLen;
    WCHAR decorated[PS_URL_LEN];
    UCHAR localAddr[16];
    UCHAR remoteAddr[16];
    UINT16 dataPort = 0;
    BOOLEAN shouldBlock = FALSE;

    if (RemotePort != 21 || !ParseFtpCommand(Buffer, BytesCopied, ftpCommand, ARRAYSIZE(ftpCommand))) {
        return FALSE;
    }

    UpdateFtpSessionState(FlowHandle, ftpCommand, FALSE, 0);
    RtlStringCchPrintfW(decorated, ARRAYSIZE(decorated), L"CMD %s", ftpCommand);

    BuildAddressBytesFromStream(InFixedValues, IsV6, localAddr, remoteAddr);
    UpdateFtpDataExpectationCommand(ProcessId, IsV6, remoteAddr, ftpCommand);
    UpdateFtpActiveFlowCommand(ProcessId, IsV6, remoteAddr, ftpCommand);
    if (!IsV6 && ParseFtpPortCommand(ftpCommand, &dataPort)) {
        RegisterFtpDataExpectation(ProcessId, FALSE, FALSE, dataPort, 0, localAddr, remoteAddr, ftpCommand);
    } else if (ParseFtpEprtCommand(ftpCommand, &dataPort)) {
        RegisterFtpDataExpectation(ProcessId, IsV6, FALSE, dataPort, 0, localAddr, remoteAddr, ftpCommand);
    }

    if (!ShouldSuppressDuplicate(FlowHandle, StreamSignalFtpLine, HashWideString(decorated))) {
        event = NetEventAllocate();
        if (event == NULL) {
            return FALSE;
        }
        if (FinalizeInspectionEvent(
                event,
                ProcessId,
                RemotePort,
                LocalPort,
                EventFtpCommand,
                ftpCommand,
                InMetaValues) == ActionBlocked) {
            shouldBlock = TRUE;
        }

        if (IsV6) CopyAddressesV6(InFixedValues, event);
        else CopyAddressesV4(InFixedValues, event);

        commandLen = wcsnlen(decorated, ARRAYSIZE(event->Url) - 1);
        event->UrlLength = (USHORT)commandLen;
        RtlCopyMemory(event->Url, decorated, commandLen * sizeof(WCHAR));
        event->Url[commandLen] = L'\0';
        NetEventEnqueue(event);
        NetEventFree(event);
    }

    return shouldBlock;
}

static
BOOLEAN
TryEnqueueFtpResponseEvent(
    _In_reads_(BytesCopied) const UCHAR* Buffer,
    _In_ ULONG BytesCopied,
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ BOOLEAN IsV6
    )
{
    PFTP_RESPONSE_SCRATCH scratch;
    BOOLEAN authenticated = FALSE;
    BOOLEAN transferInProgress = FALSE;
    BOOLEAN transferDataSeen = FALSE;
    ULONG responseCode = 0;
    PDLP_NET_EVENT event;
    size_t lineLen;
    UCHAR localAddr[16];
    UCHAR remoteAddr[16];
    UINT16 dataPort = 0;
    BOOLEAN shouldBlock = FALSE;

    if (RemotePort != 21) {
        return FALSE;
    }
    scratch = (PFTP_RESPONSE_SCRATCH)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*scratch),
        'rFsP');
    if (scratch == NULL) return FALSE;
    RtlZeroMemory(scratch, sizeof(*scratch));
    if (!ParseFtpResponse(
            Buffer,
            BytesCopied,
            scratch->Response,
            ARRAYSIZE(scratch->Response),
            &responseCode)) {
        ExFreePool(scratch);
        return FALSE;
    }

    GetFtpSessionState(
        FlowHandle,
        scratch->LastCommand,
        ARRAYSIZE(scratch->LastCommand),
        &authenticated,
        scratch->PendingTransfer,
        ARRAYSIZE(scratch->PendingTransfer),
        &transferInProgress,
        &transferDataSeen);
    UpdateFtpSessionState(FlowHandle, NULL, TRUE, responseCode);
    if (responseCode == 230) authenticated = TRUE;
    else if (responseCode == 221 || responseCode == 421 || responseCode == 530) authenticated = FALSE;

    if ((responseCode == 226 || responseCode == 250 ||
         responseCode == 425 || responseCode == 426 ||
         responseCode == 450 || responseCode == 451 ||
         responseCode == 452 || responseCode == 550 ||
         responseCode == 551 || responseCode == 552 ||
         responseCode == 553) &&
        (scratch->PendingTransfer[0] != L'\0' || transferDataSeen)) {
        QueryAndClearFtpTransferSummary(
            ProcessId,
            scratch->PendingTransfer[0] != L'\0' ? scratch->PendingTransfer : scratch->LastCommand,
            TRUE,
            &scratch->TransferSummary);
    }

    if ((responseCode == 125 || responseCode == 150) && scratch->PendingTransfer[0] != L'\0') {
        RtlStringCchPrintfW(
            scratch->Decorated,
            ARRAYSIZE(scratch->Decorated),
            L"TRANSFER START %s %s",
            scratch->PendingTransfer,
            authenticated ? L"AUTH" : L"NOAUTH");
    } else if ((responseCode == 226 || responseCode == 250) && (scratch->PendingTransfer[0] != L'\0' || transferDataSeen)) {
        RtlStringCchPrintfW(
            scratch->Decorated,
            ARRAYSIZE(scratch->Decorated),
            L"TRANSFER COMPLETE %s %s %I64u bytes missedBytes=%I64u entries=%lu files=%lu dirs=%lu links=%lu listedBytes=%I64u suppressed=%lu truncated=%u",
            scratch->PendingTransfer[0] != L'\0' ? scratch->PendingTransfer : scratch->LastCommand,
            authenticated ? L"AUTH" : L"NOAUTH",
            scratch->TransferSummary.TotalBytesObserved,
            scratch->TransferSummary.TotalBytesMissed,
            scratch->TransferSummary.DirectoryEntryCount,
            scratch->TransferSummary.DirectoryFileCount,
            scratch->TransferSummary.DirectoryDirectoryCount,
            scratch->TransferSummary.DirectoryLinkCount,
            scratch->TransferSummary.ListedFileBytes,
            scratch->TransferSummary.DirectoryEntryEventsSuppressed,
            scratch->TransferSummary.DirectoryLineOverflow ? 1u : 0u);
    } else if ((responseCode == 425 || responseCode == 426 ||
                responseCode == 450 || responseCode == 451 ||
                responseCode == 452 || responseCode == 550 ||
                responseCode == 551 || responseCode == 552 ||
                responseCode == 553) && (scratch->PendingTransfer[0] != L'\0' || transferInProgress || transferDataSeen)) {
        RtlStringCchPrintfW(
            scratch->Decorated,
            ARRAYSIZE(scratch->Decorated),
            L"TRANSFER FAIL %lu %s %s %I64u bytes missedBytes=%I64u entries=%lu files=%lu dirs=%lu links=%lu listedBytes=%I64u suppressed=%lu truncated=%u",
            responseCode,
            scratch->PendingTransfer[0] != L'\0' ? scratch->PendingTransfer : scratch->LastCommand,
            authenticated ? L"AUTH" : L"NOAUTH",
            scratch->TransferSummary.TotalBytesObserved,
            scratch->TransferSummary.TotalBytesMissed,
            scratch->TransferSummary.DirectoryEntryCount,
            scratch->TransferSummary.DirectoryFileCount,
            scratch->TransferSummary.DirectoryDirectoryCount,
            scratch->TransferSummary.DirectoryLinkCount,
            scratch->TransferSummary.ListedFileBytes,
            scratch->TransferSummary.DirectoryEntryEventsSuppressed,
            scratch->TransferSummary.DirectoryLineOverflow ? 1u : 0u);
    } else {
        RtlStringCchPrintfW(
            scratch->Decorated,
            ARRAYSIZE(scratch->Decorated),
            L"RESP %lu %s %s",
            responseCode,
            authenticated ? L"AUTH" : L"NOAUTH",
            scratch->LastCommand[0] != L'\0' ? scratch->LastCommand : L"");
    }

    BuildAddressBytesFromStream(InFixedValues, IsV6, localAddr, remoteAddr);
    if (responseCode == 227 && !IsV6 && ParseFtpPasvResponse(scratch->Response, &dataPort) &&
        GetFlowControlInfo(FlowHandle, &scratch->ControlInfo)) {
        RegisterFtpDataExpectation(
            ProcessId,
            FALSE,
            TRUE,
            0,
            dataPort,
            scratch->ControlInfo.LocalAddress,
            scratch->ControlInfo.RemoteAddress,
            scratch->LastCommand);
    } else if (responseCode == 229 && ParseFtpEpsvResponse(scratch->Response, &dataPort) &&
        GetFlowControlInfo(FlowHandle, &scratch->ControlInfo)) {
        RegisterFtpDataExpectation(
            ProcessId,
            scratch->ControlInfo.IsV6,
            TRUE,
            0,
            dataPort,
            scratch->ControlInfo.LocalAddress,
            scratch->ControlInfo.RemoteAddress,
            scratch->LastCommand);
    }

    if (!ShouldSuppressDuplicate(FlowHandle, StreamSignalFtpLine, HashWideString(scratch->Decorated))) {
        event = NetEventAllocate();
        if (event == NULL) {
            ExFreePool(scratch);
            return FALSE;
        }
        if (FinalizeInspectionEvent(
                event,
                ProcessId,
                RemotePort,
                LocalPort,
                EventFtpCommand,
                scratch->Decorated,
                InMetaValues) == ActionBlocked) {
            shouldBlock = TRUE;
        }

        if (IsV6) CopyAddressesV6(InFixedValues, event);
        else CopyAddressesV4(InFixedValues, event);

        lineLen = wcsnlen(scratch->Decorated, ARRAYSIZE(event->Url) - 1);
        event->UrlLength = (USHORT)lineLen;
        RtlCopyMemory(event->Url, scratch->Decorated, lineLen * sizeof(WCHAR));
        event->Url[lineLen] = L'\0';
        NetEventEnqueue(event);
        NetEventFree(event);
    }

    ExFreePool(scratch);
    return shouldBlock;
}

static
VOID
ApplyStreamBlockDecision(
    _Inout_ FWPS_STREAM_CALLOUT_IO_PACKET0* IoPacket,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    IoPacket->countBytesRequired = 0;
    if (IoPacket->streamData != NULL) {
        IoPacket->countBytesEnforced = IoPacket->streamData->dataLength;
    }
    IoPacket->streamAction = FWPS_STREAM_ACTION_DROP_CONNECTION;
    if ((ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) != 0) {
        ClassifyOut->actionType = FWP_ACTION_BLOCK;
        ClassifyOut->flags |= FWPS_CLASSIFY_OUT_FLAG_ABSORB;
        ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
    }
}

static
VOID
MarkFtpTransferDataSeen(
    _In_ UINT64 FlowHandle
    )
{
    KIRQL oldIrql;
    PSTREAM_FLOW_CACHE_ENTRY entry;

    if (FlowHandle == 0) {
        return;
    }

    EnsureCachesInitialized();
    KeAcquireSpinLock(&gStreamFlowCacheLock, &oldIrql);
    entry = GetOrCreateFlowEntryLocked(FlowHandle);
    entry->TransferDataSeen = TRUE;
    entry->TransferInProgress = TRUE;
    KeQuerySystemTime(&entry->LastSeen);
    KeReleaseSpinLock(&gStreamFlowCacheLock, oldIrql);
}

static
BOOLEAN
TryBuildFtpDataEventCommon(
    _In_ UINT64 FlowHandle,
    _In_ ULONG ProcessId,
    _In_ BOOLEAN IsV6,
    _In_ BOOLEAN OutboundObserved,
    _In_ UINT16 LocalPort,
    _In_ UINT16 RemotePort,
    _In_reads_(16) const UCHAR* LocalAddr,
    _In_reads_(16) const UCHAR* RemoteAddr,
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Out_ PDLP_NET_EVENT Event
    )
{
    WCHAR command[PS_URL_LEN];
    WCHAR decorated[PS_URL_LEN];
    size_t len;

    if (!MatchFtpDataExpectation(
            ProcessId,
            IsV6,
            OutboundObserved,
            LocalPort,
            RemotePort,
            LocalAddr,
            RemoteAddr,
            command,
            ARRAYSIZE(command))) {
        return FALSE;
    }

    MarkFtpTransferDataSeen(FlowHandle);
    RegisterFtpActiveFlow(
        ProcessId,
        IsV6,
        OutboundObserved,
        LocalPort,
        RemotePort,
        LocalAddr,
        RemoteAddr,
        command);

    RtlZeroMemory(Event, sizeof(*Event));
    RtlStringCchPrintfW(
        decorated,
        ARRAYSIZE(decorated),
        L"DATA %s %s",
        OutboundObserved ? L"OUT" : L"IN",
        command[0] != L'\0' ? command : L"UNKNOWN");

    FinalizeInspectionEvent(
        Event,
        ProcessId,
        RemotePort,
        LocalPort,
        EventFtpCommand,
        command[0] != L'\0' ? command : L"DATA",
        InMetaValues);

    if (IsV6) CopyAddressesV6(InFixedValues, Event);
    else CopyAddressesV4(InFixedValues, Event);

    len = wcsnlen(decorated, ARRAYSIZE(Event->Url) - 1);
    Event->UrlLength = (USHORT)len;
    RtlCopyMemory(Event->Url, decorated, len * sizeof(WCHAR));
    Event->Url[len] = L'\0';
    return TRUE;
}

BOOLEAN
ClassificationTryMatchFtpDataChannelV4(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _In_ DLP_EVENT_TYPE eventType,
    _Out_ PDLP_NET_EVENT event
    )
{
    ULONG processId = 0;
    BOOLEAN outboundObserved = (eventType == EventNetworkConnect);
    UCHAR localAddr[16] = { 0 };
    UCHAR remoteAddr[16] = { 0 };
    UINT16 localPort;
    UINT16 remotePort;

    if (event == NULL || inFixedValues == NULL) {
        return FALSE;
    }

    if (inMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(inMetaValues, FWPS_METADATA_FIELD_PROCESS_ID)) {
        processId = (ULONG)inMetaValues->processId;
    }
    UINT64 flowHandle = 0;
    if (inMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(inMetaValues, FWPS_METADATA_FIELD_FLOW_HANDLE)) {
        flowHandle = inMetaValues->flowHandle;
    }

    localPort = inFixedValues->incomingValue[
        outboundObserved
            ? FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_PORT
            : FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_LOCAL_PORT].value.uint16;
    remotePort = inFixedValues->incomingValue[
        outboundObserved
            ? FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_PORT
            : FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_REMOTE_PORT].value.uint16;

    CopyAddressBytesV4(
        inFixedValues->incomingValue[
            outboundObserved
                ? FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_ADDRESS
                : FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_LOCAL_ADDRESS].value.uint32,
        localAddr);
    CopyAddressBytesV4(
        inFixedValues->incomingValue[
            outboundObserved
                ? FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS
                : FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_REMOTE_ADDRESS].value.uint32,
        remoteAddr);

    return TryBuildFtpDataEventCommon(
        flowHandle,
        processId,
        FALSE,
        outboundObserved,
        localPort,
        remotePort,
        localAddr,
        remoteAddr,
        inFixedValues,
        inMetaValues,
        event);
}

BOOLEAN
ClassificationTryMatchFtpDataChannelV6(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _In_ DLP_EVENT_TYPE eventType,
    _Out_ PDLP_NET_EVENT event
    )
{
    ULONG processId = 0;
    BOOLEAN outboundObserved = (eventType == EventNetworkConnect);
    UCHAR localAddr[16];
    UCHAR remoteAddr[16];
    UINT16 localPort;
    UINT16 remotePort;

    if (event == NULL || inFixedValues == NULL) {
        return FALSE;
    }

    if (inMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(inMetaValues, FWPS_METADATA_FIELD_PROCESS_ID)) {
        processId = (ULONG)inMetaValues->processId;
    }
    UINT64 flowHandle = 0;
    if (inMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(inMetaValues, FWPS_METADATA_FIELD_FLOW_HANDLE)) {
        flowHandle = inMetaValues->flowHandle;
    }

    localPort = inFixedValues->incomingValue[
        outboundObserved
            ? FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_LOCAL_PORT
            : FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_LOCAL_PORT].value.uint16;
    remotePort = inFixedValues->incomingValue[
        outboundObserved
            ? FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_REMOTE_PORT
            : FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_REMOTE_PORT].value.uint16;

    CopyAddressBytesV6(
        inFixedValues->incomingValue[
            outboundObserved
                ? FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_LOCAL_ADDRESS
                : FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_LOCAL_ADDRESS].value.byteArray16,
        localAddr);
    CopyAddressBytesV6(
        inFixedValues->incomingValue[
            outboundObserved
                ? FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_REMOTE_ADDRESS
                : FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_REMOTE_ADDRESS].value.byteArray16,
        remoteAddr);

    return TryBuildFtpDataEventCommon(
        flowHandle,
        processId,
        TRUE,
        outboundObserved,
        localPort,
        remotePort,
        localAddr,
        remoteAddr,
        inFixedValues,
        inMetaValues,
        event);
}

static
VOID
InspectStreamPayload(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_ VOID* LayerData,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut,
    _In_ BOOLEAN IsV6
    )
{
    FWPS_STREAM_CALLOUT_IO_PACKET0* ioPacket;
    FWPS_STREAM_DATA0* streamData;
    FWP_DIRECTION direction;
    ULONG processId = 0;
    UINT16 remotePort;
    UINT16 localPort;
    UINT64 flowHandle = 0;
    PUCHAR buffer;
    SIZE_T bytesToCopy;
    SIZE_T bytesCopied = 0;
    UCHAR localAddr[16];
    UCHAR remoteAddr[16];
    BOOLEAN block = FALSE;

    if (ClassifyOut == NULL || LayerData == NULL) {
        return;
    }

    ioPacket = (FWPS_STREAM_CALLOUT_IO_PACKET0*)LayerData;
    PrepareStreamDefaults(ioPacket, ClassifyOut);

    streamData = ioPacket->streamData;
    if (streamData == NULL) {
        return;
    }

    direction = GetPacketDirection(InFixedValues, IsV6);
    if (direction != FWP_DIRECTION_OUTBOUND && direction != FWP_DIRECTION_INBOUND) {
        return;
    }

    if (InMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_ID)) {
        processId = (ULONG)InMetaValues->processId;
    }
    if (InMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_FLOW_HANDLE)) {
        flowHandle = InMetaValues->flowHandle;
    }

    if (ProtectShouldBypassDlp(processId)) {
        ioPacket->countBytesEnforced = streamData->dataLength;
        return;
    }

    remotePort = IsV6
        ? InFixedValues->incomingValue[FWPS_FIELD_STREAM_V6_IP_REMOTE_PORT].value.uint16
        : InFixedValues->incomingValue[FWPS_FIELD_STREAM_V4_IP_REMOTE_PORT].value.uint16;
    localPort = IsV6
        ? InFixedValues->incomingValue[FWPS_FIELD_STREAM_V6_IP_LOCAL_PORT].value.uint16
        : InFixedValues->incomingValue[FWPS_FIELD_STREAM_V4_IP_LOCAL_PORT].value.uint16;

    BuildAddressBytesFromStream(InFixedValues, IsV6, localAddr, remoteAddr);
    UpdateFlowControlInfo(flowHandle, processId, IsV6, localPort, remotePort, localAddr, remoteAddr);

    if (ioPacket->missedBytes != 0) {
        UpdateFtpActiveFlowMissedBytes(
            processId,
            IsV6,
            localPort,
            remotePort,
            localAddr,
            remoteAddr,
            ioPacket->missedBytes);
    }
    if (streamData->dataLength == 0) {
        return;
    }

    bytesToCopy = streamData->dataLength < PS_STREAM_INSPECT_BYTES
        ? streamData->dataLength
        : PS_STREAM_INSPECT_BYTES;
    if (bytesToCopy == 0) {
        return;
    }

    buffer = (PUCHAR)ExAllocateFromNPagedLookasideList(&gStreamInspectBufferLookaside);
    if (buffer == NULL) {
        return;
    }

    FwpsCopyStreamDataToBuffer0(streamData, buffer, bytesToCopy, &bytesCopied);
    if (bytesCopied == 0) {
        ExFreeToNPagedLookasideList(&gStreamInspectBufferLookaside, buffer);
        return;
    }
    ioPacket->countBytesEnforced = bytesCopied;

    if (direction == FWP_DIRECTION_OUTBOUND) {
        block |= ProcessHttpStreamState(
            flowHandle, buffer, (ULONG)bytesCopied, processId, remotePort, localPort,
            InFixedValues, InMetaValues, IsV6, TRUE);
        block |= TryEnqueueSniEvent(
            buffer, (ULONG)bytesCopied, flowHandle, processId, remotePort, localPort,
            InFixedValues, InMetaValues, IsV6);
        block |= TryEnqueueFtpCommandEvent(
            buffer, (ULONG)bytesCopied, flowHandle, processId, remotePort, localPort,
            InFixedValues, InMetaValues, IsV6);
        block |= TryEnqueueFtpDataPreviewEvent(
            buffer, (ULONG)bytesCopied, processId, remotePort, localPort,
            localAddr, remoteAddr, InFixedValues, InMetaValues, IsV6, TRUE);
    } else {
        block |= ProcessHttpStreamState(
            flowHandle, buffer, (ULONG)bytesCopied, processId, remotePort, localPort,
            InFixedValues, InMetaValues, IsV6, FALSE);
        block |= TryEnqueueFtpResponseEvent(
            buffer, (ULONG)bytesCopied, flowHandle, processId, remotePort, localPort,
            InFixedValues, InMetaValues, IsV6);
        block |= TryEnqueueFtpDataPreviewEvent(
            buffer, (ULONG)bytesCopied, processId, remotePort, localPort,
            localAddr, remoteAddr, InFixedValues, InMetaValues, IsV6, FALSE);
    }

    if (block) {
        ApplyStreamBlockDecision(ioPacket, ClassifyOut);
    }
    ExFreeToNPagedLookasideList(&gStreamInspectBufferLookaside, buffer);
}

VOID NTAPI
StreamInspectOutboundV4(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _Inout_opt_ VOID* layerData,
    _In_ const FWPS_FILTER0* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* classifyOut
    )
{
    UNREFERENCED_PARAMETER(filter);
    UNREFERENCED_PARAMETER(flowContext);
    InspectStreamPayload(inFixedValues, inMetaValues, layerData, classifyOut, FALSE);
}

VOID NTAPI
StreamInspectOutboundV6(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _Inout_opt_ VOID* layerData,
    _In_ const FWPS_FILTER0* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* classifyOut
    )
{
    UNREFERENCED_PARAMETER(filter);
    UNREFERENCED_PARAMETER(flowContext);
    InspectStreamPayload(inFixedValues, inMetaValues, layerData, classifyOut, TRUE);
}

NTSTATUS
ParseHttpTraffic(
    _In_reads_(packetLength) const UCHAR *packetData,
    _In_ ULONG packetLength,
    _Out_ PWSTR *parsedUrl,
    _Out_ PULONG parsedUrlLength
    )
{
    ULONG methodLength = 0;
    ULONG pathEnd;
    ULONG pathLength;
    const UCHAR* hostValue = NULL;
    ULONG hostLength = 0;

    if (packetData == NULL || parsedUrl == NULL || parsedUrlLength == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *parsedUrl = NULL;
    *parsedUrlLength = 0;

    if (!StartsWithHttpMethod(packetData, packetLength, &methodLength)) {
        return STATUS_NOT_FOUND;
    }

    pathEnd = methodLength;
    while (pathEnd < packetLength &&
           packetData[pathEnd] != ' ' &&
           packetData[pathEnd] != '\r' &&
           packetData[pathEnd] != '\n') {
        pathEnd++;
    }

    if (pathEnd <= methodLength) {
        return STATUS_NOT_FOUND;
    }

    pathLength = pathEnd - methodLength;
    (void)FindHeaderValue(packetData, packetLength, "Host:", &hostValue, &hostLength);

    return BuildUnicodeUrl(
        hostValue,
        hostLength,
        packetData + methodLength,
        pathLength,
        parsedUrl,
        parsedUrlLength);
}
