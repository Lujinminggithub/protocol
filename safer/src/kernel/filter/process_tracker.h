/*
 * process_tracker.h - process attribution cache for file/network events
 */

#pragma once

#include <fltKernel.h>

typedef struct _PROCESS_TRACK_ENTRY {
    LIST_ENTRY ListEntry;
    ULONG ProcessId;
    PEPROCESS ProcessObject;
    WCHAR ProcessName[260];
    USHORT ProcessNameLength;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    ULONG FileOpCount;
    BOOLEAN Active;
} PROCESS_TRACK_ENTRY, *PPROCESS_TRACK_ENTRY;

typedef struct _PROCESS_TRACK_TABLE {
    LIST_ENTRY ProcessList;
    ULONG EntryCount;
    ULONG MaxEntries;
    FAST_MUTEX TableLock;
} PROCESS_TRACK_TABLE, *PPROCESS_TRACK_TABLE;

NTSTATUS
ProcessTrackerInitialize(VOID);

VOID
ProcessTrackerCleanup(VOID);

PPROCESS_TRACK_ENTRY
ProcessTrackerTrackProcess(
    _In_ PEPROCESS Process
);

VOID
ProcessTrackerUntrackProcess(
    _In_ HANDLE ProcessId
);

USHORT
ProcessTrackerGetProcessName(
    _In_ PEPROCESS Process,
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
);

USHORT
ProcessTrackerGetProcessNameById(
    _In_ HANDLE ProcessId,
    _Out_writes_to_(BufferLengthInChars, return) PWCHAR Buffer,
    _In_ USHORT BufferLengthInChars
);

ULONG
ProcessTrackerGetProcessId(
    _In_ PEPROCESS Process
);

BOOLEAN
ProcessTrackerIsSystemProcessId(
    _In_ HANDLE ProcessId
);
