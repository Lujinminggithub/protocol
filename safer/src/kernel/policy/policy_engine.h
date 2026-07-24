/*
 * policy_engine.h - kernel policy engine declarations
 */

#pragma once

#include <fltKernel.h>
#include "../common/shared_types.h"

typedef enum _FILE_ACTION_POLICY {
    FileActionNone = 0,
    FileActionLog,
    FileActionBlock,
    FileActionQuarantine
} FILE_ACTION_POLICY;

typedef enum _NET_ACTION_POLICY {
    NetActionNone = 0,
    NetActionLog,
    NetActionBlock,
    NetActionRedirect
} NET_ACTION_POLICY;

typedef struct _POLICY_ENGINE_STATE {
    PS_POLICY_DATA Policy;
    PS_QUARANTINE_SETTINGS Quarantine;
    KSPIN_LOCK Lock;
    NPAGED_LOOKASIDE_LIST SnapshotLookaside;
    BOOLEAN Initialized;
    BOOLEAN SnapshotLookasideInitialized;
} POLICY_ENGINE_STATE, *PPOLICY_ENGINE_STATE;

BOOLEAN
PolicyEngineIsProcessBlocked(
    _In_ PCWSTR ProcessName
);

NTSTATUS
PolicyEngineInitialize(VOID);

VOID
PolicyEngineCleanup(VOID);

BOOLEAN
PolicyEngineIsAuditEnabled(VOID);

NTSTATUS
PolicyEngineSetPolicy(
    _In_ PPOLICY_COMMAND Command
);

NTSTATUS
PolicyEngineGetPolicy(
    _Out_writes_bytes_(bufferSize) PVOID Buffer,
    _In_ ULONG bufferSize,
    _Out_ PULONG bytesReturned
);

NTSTATUS
PolicyEngineReload(VOID);

NTSTATUS
PolicyEngineSetQuarantineSettings(
    _In_ const PS_QUARANTINE_SETTINGS* Settings
);

NTSTATUS
PolicyEngineGetQuarantineSettings(
    _Out_ PS_QUARANTINE_SETTINGS* Settings
);

ACTION_RESULT
PolicyEngineQueryFileAction(
    _In_ ULONG ProcessId,
    _In_ PCWSTR FileName,
    _In_ DLP_EVENT_TYPE EventType
);

ACTION_RESULT
PolicyEngineQueryNetAction(
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ PCWSTR Url,
    _In_ DLP_EVENT_TYPE EventType
);
