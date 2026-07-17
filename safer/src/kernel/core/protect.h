#pragma once

#include <fltKernel.h>
#include "../common/shared_types.h"

#define PS_PROTECT_MODE_LOCKED 0UL
#define PS_PROTECT_MODE_UNLOCKED 1UL

NTSTATUS ProtectInitialize(_In_ PDRIVER_OBJECT DriverObject);
VOID ProtectCleanup(VOID);

NTSTATUS ProtectSetList(_In_ const PS_PROTECT_LIST* List);
NTSTATUS ProtectGetChallenge(_Out_ PS_PROTECT_TICKET* Ticket);
NTSTATUS ProtectUnlock(_In_ const PS_PROTECT_UNLOCK_REQUEST* Request);
VOID ProtectRelock(VOID);
VOID ProtectQueryState(_Out_ PS_PROTECT_STATUS* Status);

BOOLEAN ProtectShouldEnforce(VOID);
BOOLEAN ProtectCanUnload(VOID);
BOOLEAN ProtectIsPathProtected(_In_z_ const WCHAR* Path);
BOOLEAN ProtectIsProcessProtected(_In_ ULONG ProcessId);
BOOLEAN ProtectIsMaintenanceProcess(_In_ ULONG ProcessId);
BOOLEAN ProtectShouldBypassDlp(_In_ ULONG ProcessId);
VOID ProtectObserveProcess(_In_ PEPROCESS Process);
VOID ProtectProcessStarted(_In_ ULONG ProcessId, _In_opt_ PCUNICODE_STRING ImagePath);
VOID ProtectProcessStopped(_In_ ULONG ProcessId);
NTSTATUS ProtectControl(_In_ const PS_PROTECT_CONTROL* Control);
