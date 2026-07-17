/*
 * driver.c - PersonalSafer kernel driver entry/unload
 */

#include "driver.h"
#include "device.h"
#include "ioctls.h"
#include "common/shared_types.h"
#include "common/shared_events.h"
#include "filter/fltmgr.h"
#include "filter/process_tracker.h"
#include "filter/file_event.h"
#include "network/net_event.h"
#include "network/classification.h"
#include "network/wfp_callout.h"
#include "policy/policy_engine.h"
#include "core/protect.h"

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers.")

PFLT_FILTER gFilterHandle = NULL;
BOOLEAN gDriverStarted = FALSE;
BOOLEAN gDriverAcceptingIo = FALSE;
LARGE_INTEGER gDriverStartTime = { 0 };
IO_REMOVE_LOCK gDeviceRemoveLock;

CONST FLT_CONTEXT_REGISTRATION gContextRegistration[] = {
    { FLT_STREAMHANDLE_CONTEXT, 0, QuarantineWriteContextCleanup, sizeof(PS_QUARANTINE_WRITE_CONTEXT), 'qspS' },
    { FLT_CONTEXT_END }
};

CONST FLT_OPERATION_REGISTRATION gCallbacks[] = {
    { IRP_MJ_CREATE,             0, PreCreate,            PostCreate },
    { IRP_MJ_READ,               0, PreRead,              NULL },
    { IRP_MJ_WRITE,              0, PreWrite,             NULL },
    { IRP_MJ_QUERY_INFORMATION,  0, PreQueryInformation,  NULL },
    { IRP_MJ_DIRECTORY_CONTROL,  0, PreDirectoryControl,  PostDirectoryControl },
    { IRP_MJ_SET_INFORMATION,    0, PreSetInformation,    NULL },
    { IRP_MJ_OPERATION_END }
};

CONST FLT_REGISTRATION gFilterRegistration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    gContextRegistration,
    gCallbacks,
    PsFilterUnload,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;

    PAGED_CODE();
    DLP_LOG(DLP_DEBUG_INFO, "PersonalSafer DriverEntry started");

    status = DeviceCreate(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "DeviceCreate failed: 0x%x", status);
        return status;
    }

    status = ProtectInitialize(DriverObject);
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "ProtectInitialize failed: 0x%x", status);
        DeviceCleanup();
        return status;
    }

    status = ProcessTrackerInitialize();
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "ProcessTrackerInitialize failed: 0x%x", status);
        ProtectCleanup();
        DeviceCleanup();
        return status;
    }

    status = FileEventInitialize();
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "FileEventInitialize failed: 0x%x", status);
        ProcessTrackerCleanup();
        ProtectCleanup();
        DeviceCleanup();
        return status;
    }

    status = NetEventInitialize();
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "NetEventInitialize failed: 0x%x", status);
        FileEventCleanup();
        ProcessTrackerCleanup();
        ProtectCleanup();
        DeviceCleanup();
        return status;
    }

    status = PolicyEngineInitialize();
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "PolicyEngineInitialize failed: 0x%x", status);
        NetEventCleanup();
        FileEventCleanup();
        ProcessTrackerCleanup();
        ProtectCleanup();
        DeviceCleanup();
        return status;
    }

    status = ClassificationInitialize();
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "ClassificationInitialize failed: 0x%x", status);
        PolicyEngineCleanup();
        NetEventCleanup();
        FileEventCleanup();
        ProcessTrackerCleanup();
        ProtectCleanup();
        DeviceCleanup();
        return status;
    }

    status = WfpCalloutInitialize();
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "WfpCalloutInitialize failed: 0x%x", status);
        ProtectCleanup();
        ClassificationCleanup();
        PolicyEngineCleanup();
        NetEventCleanup();
        FileEventCleanup();
        ProcessTrackerCleanup();
        DeviceCleanup();
        return status;
    }

    /* Start file callbacks last. Every callback can now rely on process/system
       bypass state, policy storage, event queues, and self-protection. */
    status = FltRegisterFilter(DriverObject, &gFilterRegistration, &gFilterHandle);
    if (NT_SUCCESS(status)) {
        status = FltStartFiltering(gFilterHandle);
        if (NT_SUCCESS(status)) {
            gDriverStarted = TRUE;
            KeQuerySystemTime(&gDriverStartTime);
            DLP_LOG(DLP_DEBUG_INFO, "MiniFilter started successfully");
        } else {
            FltUnregisterFilter(gFilterHandle);
            gFilterHandle = NULL;
        }
    }

    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "MiniFilter registration failed: 0x%x", status);
        WfpCalloutCleanup();
        ProtectCleanup();
        ClassificationCleanup();
        PolicyEngineCleanup();
        NetEventCleanup();
        FileEventCleanup();
        ProcessTrackerCleanup();
        DeviceCleanup();
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
PsFilterUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
{
    PAGED_CODE();
    DLP_LOG(DLP_DEBUG_INFO, "PsFilterUnload called");

    if (!FlagOn(Flags, FLTFL_FILTER_UNLOAD_MANDATORY) && !ProtectCanUnload()) {
        DLP_LOG(DLP_DEBUG_WARN, "Protected unload rejected while locked");
        return STATUS_FLT_DO_NOT_DETACH;
    }

    gDriverAcceptingIo = FALSE;
    DeviceBeginUnload();

    /* Stop callback producers before destroying state they consult. */
    if (gFilterHandle != NULL) {
        FltUnregisterFilter(gFilterHandle);
        gFilterHandle = NULL;
    }

    gDriverStarted = FALSE;

    WfpCalloutCleanup();
    ProtectCleanup();
    ClassificationCleanup();
    FltMgrCleanup();
    PolicyEngineCleanup();
    NetEventCleanup();
    FileEventCleanup();
    ProcessTrackerCleanup();
    DeviceCleanup();

    DLP_LOG(DLP_DEBUG_INFO, "Driver unloaded");
    return STATUS_SUCCESS;
}
