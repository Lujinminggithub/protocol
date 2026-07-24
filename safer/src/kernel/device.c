/*
 * device.c - device creation and IRP dispatch
 */

#include "device.h"
#include "driver.h"
#include "ioctls.h"
#include "common/shared_types.h"
#include "common/shared_events.h"
#include "filter/file_event.h"
#include "network/net_event.h"
#include "network/wfp_callout.h"
#include "policy/policy_engine.h"
#include "core/protect.h"
#include <wdmsec.h>

PDEVICE_OBJECT gDeviceObject = NULL;

static UNICODE_STRING gDeviceName = RTL_CONSTANT_STRING(PS_DEVICE_NAME);
static BOOLEAN gDeviceSymLinkCreated = FALSE;
static const GUID gPersonalSaferDeviceClass =
    { 0x8ea63c1e, 0x6337, 0x49d9, { 0xa3, 0x94, 0x4f, 0x8d, 0xc5, 0xe8, 0x1f, 0x31 } };
static UNICODE_STRING gDeviceSddl = RTL_CONSTANT_STRING(
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)");

static
VOID
DeviceDeleteSymbolicLink(VOID)
{
    UNICODE_STRING deviceSymLinkName;

    if (!gDeviceSymLinkCreated) {
        return;
    }

    RtlInitUnicodeString(&deviceSymLinkName, PS_SYMLINK_NAME);
    IoDeleteSymbolicLink(&deviceSymLinkName);
    gDeviceSymLinkCreated = FALSE;
}

static
NTSTATUS
DeviceAcquireRemoveLock(
    _In_ PIRP Irp
    )
{
    return IoAcquireRemoveLock(&gDeviceRemoveLock, Irp);
}

static
VOID
DeviceReleaseRemoveLock(
    _In_ PIRP Irp
    )
{
    IoReleaseRemoveLock(&gDeviceRemoveLock, Irp);
}

NTSTATUS
DeviceCreate(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    UNICODE_STRING deviceSymLinkName;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    status = IoCreateDeviceSecure(
        DriverObject,
        0,
        &gDeviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &gDeviceSddl,
        &gPersonalSaferDeviceClass,
        &gDeviceObject);
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "IoCreateDevice failed: 0x%x", status);
        return status;
    }

    gDeviceObject->Flags |= DO_BUFFERED_IO;
    IoInitializeRemoveLock(&gDeviceRemoveLock, 'dRpS', 0, 0);
    gDriverAcceptingIo = TRUE;

    RtlInitUnicodeString(&deviceSymLinkName, PS_SYMLINK_NAME);
    status = IoCreateSymbolicLink(&deviceSymLinkName, &gDeviceName);
    if (status == STATUS_OBJECT_NAME_COLLISION || status == STATUS_OBJECT_NAME_EXISTS) {
        // A failed load from an older build could leave this product-owned link behind.
        DLP_LOG(DLP_DEBUG_WARN, "Removing stale device symbolic link: 0x%x", status);
        IoDeleteSymbolicLink(&deviceSymLinkName);
        status = IoCreateSymbolicLink(&deviceSymLinkName, &gDeviceName);
    }
    if (!NT_SUCCESS(status)) {
        DLP_LOG(DLP_DEBUG_ERROR, "IoCreateSymbolicLink failed: 0x%x", status);
        gDriverAcceptingIo = FALSE;
        IoDeleteDevice(gDeviceObject);
        gDeviceObject = NULL;
        return status;
    }
    gDeviceSymLinkCreated = TRUE;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = DeviceDispatchUnhandled;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DeviceDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = DeviceDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DeviceDispatchClose;

    gDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DLP_LOG(DLP_DEBUG_INFO, "Device created successfully: %wZ", &deviceSymLinkName);
    return STATUS_SUCCESS;
}

VOID
DeviceBeginUnload(VOID)
{
    gDriverAcceptingIo = FALSE;
    DeviceDeleteSymbolicLink();
    if (gDeviceObject != NULL) {
        IoReleaseRemoveLockAndWait(&gDeviceRemoveLock, gDeviceObject);
    }
}

VOID
DeviceCleanup(VOID)
{
    gDriverAcceptingIo = FALSE;
    DeviceDeleteSymbolicLink();

    if (gDeviceObject != NULL) {
        IoDeleteDevice(gDeviceObject);
        gDeviceObject = NULL;
    }

    DLP_LOG(DLP_DEBUG_INFO, "Device cleaned up");
}

NTSTATUS
DeviceDispatchCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);
    status = DeviceAcquireRemoveLock(Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    Irp->IoStatus.Status = gDriverAcceptingIo ? STATUS_SUCCESS : STATUS_DELETE_PENDING;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    DeviceReleaseRemoveLock(Irp);
    return Irp->IoStatus.Status;
}

NTSTATUS
DeviceDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    ULONG ioctlCode;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);

    status = DeviceAcquireRemoveLock(Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    if (!gDriverAcceptingIo) {
        Irp->IoStatus.Status = STATUS_DELETE_PENDING;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        DeviceReleaseRemoveLock(Irp);
        return STATUS_DELETE_PENDING;
    }

    stack = IoGetCurrentIrpStackLocation(Irp);
    ioctlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    status = STATUS_SUCCESS;

    switch (ioctlCode) {
        case IOCTL_PS_DRIVER_STATUS:
            status = HandleDriverStatusIrp(Irp);
            break;

        case IOCTL_PS_SET_POLICY:
            status = HandleSetPolicyIrp(Irp);
            break;

        case IOCTL_PS_GET_POLICY:
            status = HandleGetPolicyIrp(Irp);
            break;

        case IOCTL_PS_SET_QUARANTINE:
            status = HandleSetQuarantineIrp(Irp);
            break;

        case IOCTL_PS_GET_QUARANTINE:
            status = HandleGetQuarantineIrp(Irp);
            break;

        case IOCTL_PS_SET_REDIRECT:
            status = HandleSetRedirectIrp(Irp);
            break;

        case IOCTL_PS_QUERY_REDIRECT:
            status = HandleQueryRedirectIrp(Irp);
            break;

        case IOCTL_PS_FILE_EVENT:
            status = HandleFileEventIrp(Irp);
            break;

        case IOCTL_PS_FILE_EVENT_BATCH:
            status = HandleFileEventBatchIrp(Irp);
            break;

        case IOCTL_PS_NET_EVENT:
            status = HandleNetEventIrp(Irp);
            break;

        case IOCTL_PS_NET_EVENT_BATCH:
            status = HandleNetEventBatchIrp(Irp);
            break;

        case IOCTL_PS_WAIT_EVENTS:
            status = HandleWaitEventsIrp(Irp);
            break;

        case IOCTL_PS_SET_PROTECT_LIST:
            status = HandleSetProtectListIrp(Irp);
            break;

        case IOCTL_PS_GET_PROTECT_CHALLENGE:
            status = HandleGetProtectChallengeIrp(Irp);
            break;

        case IOCTL_PS_UNLOCK_PROTECTION:
            status = HandleUnlockProtectionIrp(Irp);
            break;

        case IOCTL_PS_RELOCK_PROTECTION:
            ProtectRelock();
            status = STATUS_SUCCESS;
            Irp->IoStatus.Information = 0;
            break;

        case IOCTL_PS_QUERY_PROTECT_STATE:
            status = HandleQueryProtectStateIrp(Irp);
            break;

        case IOCTL_PS_PROTECT_CONTROL:
            status = HandleProtectControlIrp(Irp);
            break;

        case IOCTL_PS_HEARTBEAT:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            DeviceReleaseRemoveLock(Irp);
            return STATUS_SUCCESS;

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            Irp->IoStatus.Information = 0;
            break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    DeviceReleaseRemoveLock(Irp);
    return status;
}

NTSTATUS
DeviceDispatchCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);
    status = DeviceAcquireRemoveLock(Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    DeviceReleaseRemoveLock(Irp);
    return STATUS_SUCCESS;
}

NTSTATUS
DeviceDispatchClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);
    status = DeviceAcquireRemoveLock(Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    DeviceReleaseRemoveLock(Irp);
    return STATUS_SUCCESS;
}

NTSTATUS
DeviceDispatchUnhandled(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);
    status = DeviceAcquireRemoveLock(Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    DeviceReleaseRemoveLock(Irp);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
HandleDriverStatusIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PDRIVER_STATUS statusData;

    stack = IoGetCurrentIrpStackLocation(Irp);
    statusData = (PDRIVER_STATUS)Irp->AssociatedIrp.SystemBuffer;

    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DRIVER_STATUS)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(statusData, sizeof(DRIVER_STATUS));
    statusData->DriverLoaded = gDriverStarted;
    statusData->FileFilterActive = gDriverStarted;
    statusData->NetworkFilterActive = WfpCalloutIsInitialized();
    statusData->LastErrorCode = (ULONG)WfpCalloutGetLastError();
    statusData->StartTime = gDriverStartTime;
    statusData->FileQueueDepth = FileEventGetQueueLength();
    statusData->NetQueueDepth = NetEventGetQueueLength();
    statusData->FileDroppedEvents = FileEventGetDroppedCount();
    statusData->NetDroppedEvents = NetEventGetDroppedCount();
    statusData->TotalEvents =
        statusData->FileQueueDepth +
        statusData->NetQueueDepth +
        statusData->FileDroppedEvents +
        statusData->NetDroppedEvents;

    Irp->IoStatus.Information = sizeof(DRIVER_STATUS);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleSetPolicyIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PPOLICY_COMMAND policyCmd;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    policyCmd = (PPOLICY_COMMAND)Irp->AssociatedIrp.SystemBuffer;

    if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(POLICY_COMMAND)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = PolicyEngineSetPolicy(policyCmd);
    Irp->IoStatus.Information = 0;
    return status;
}

NTSTATUS
HandleGetPolicyIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PPOLICY_COMMAND policyCmd;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    policyCmd = (PPOLICY_COMMAND)Irp->AssociatedIrp.SystemBuffer;

    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(POLICY_COMMAND)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(policyCmd, sizeof(POLICY_COMMAND));
    policyCmd->CommandId = CMD_GET_POLICY;
    status = PolicyEngineGetPolicy(
        policyCmd->PolicyData,
        sizeof(policyCmd->PolicyData),
        &policyCmd->PolicySize);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    Irp->IoStatus.Information = sizeof(POLICY_COMMAND);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleFileEventIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PDLP_FILE_EVENT event;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DLP_FILE_EVENT)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    event = (PDLP_FILE_EVENT)Irp->AssociatedIrp.SystemBuffer;
    status = FileEventDequeue(event, 0);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    Irp->IoStatus.Information = sizeof(DLP_FILE_EVENT);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleSetQuarantineIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PS_QUARANTINE_SETTINGS* settings;

    stack = IoGetCurrentIrpStackLocation(Irp);
    settings = (PS_QUARANTINE_SETTINGS*)Irp->AssociatedIrp.SystemBuffer;

    if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(PS_QUARANTINE_SETTINGS)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    Irp->IoStatus.Information = 0;
    return PolicyEngineSetQuarantineSettings(settings);
}

NTSTATUS
HandleFileEventBatchIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PDLP_FILE_EVENT_BATCH batch;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DLP_FILE_EVENT_BATCH)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    batch = (PDLP_FILE_EVENT_BATCH)Irp->AssociatedIrp.SystemBuffer;
    status = FileEventDequeueBatch(batch);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    Irp->IoStatus.Information = sizeof(*batch);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleGetQuarantineIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PS_QUARANTINE_SETTINGS* settings;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    settings = (PS_QUARANTINE_SETTINGS*)Irp->AssociatedIrp.SystemBuffer;

    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS_QUARANTINE_SETTINGS)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(settings, sizeof(*settings));
    status = PolicyEngineGetQuarantineSettings(settings);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    Irp->IoStatus.Information = sizeof(*settings);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleSetRedirectIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PS_REDIRECT_SETTINGS* settings;

    stack = IoGetCurrentIrpStackLocation(Irp);
    settings = (PS_REDIRECT_SETTINGS*)Irp->AssociatedIrp.SystemBuffer;
    if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(PS_REDIRECT_SETTINGS)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    Irp->IoStatus.Information = 0;
    return WfpRedirectSetSettings(settings);
}

NTSTATUS
HandleQueryRedirectIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PS_REDIRECT_QUERY* query;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    query = (PS_REDIRECT_QUERY*)Irp->AssociatedIrp.SystemBuffer;
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS_REDIRECT_QUERY)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = WfpRedirectQueryOriginalDestination(query);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    Irp->IoStatus.Information = sizeof(*query);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleNetEventIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PDLP_NET_EVENT event;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DLP_NET_EVENT)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    event = (PDLP_NET_EVENT)Irp->AssociatedIrp.SystemBuffer;
    status = NetEventDequeue(event, 0);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    Irp->IoStatus.Information = sizeof(DLP_NET_EVENT);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleNetEventBatchIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PDLP_NET_EVENT_BATCH batch;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DLP_NET_EVENT_BATCH)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    batch = (PDLP_NET_EVENT_BATCH)Irp->AssociatedIrp.SystemBuffer;
    status = NetEventDequeueBatch(batch);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    Irp->IoStatus.Information = sizeof(*batch);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleWaitEventsIrp(
    _In_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    PPS_EVENT_WAIT_REQUEST waitRequest;
    PVOID waitObjects[2];
    KWAIT_BLOCK waitBlocks[2];
    LARGE_INTEGER timeout;
    PLARGE_INTEGER timeoutPtr = NULL;
    NTSTATUS status;

    stack = IoGetCurrentIrpStackLocation(Irp);
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS_EVENT_WAIT_REQUEST) ||
        stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(PS_EVENT_WAIT_REQUEST)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    waitRequest = (PPS_EVENT_WAIT_REQUEST)Irp->AssociatedIrp.SystemBuffer;
    waitRequest->ReadyMask = 0;

    if (FileEventGetQueueLength() > 0) {
        waitRequest->ReadyMask |= PS_EVENT_FILE_READY;
    }
    if (NetEventGetQueueLength() > 0) {
        waitRequest->ReadyMask |= PS_EVENT_NET_READY;
    }
    if (waitRequest->ReadyMask != 0) {
        Irp->IoStatus.Information = sizeof(*waitRequest);
        return STATUS_SUCCESS;
    }

    waitObjects[0] = FileEventGetWaitEvent();
    waitObjects[1] = NetEventGetWaitEvent();

    if (waitRequest->TimeoutMs > 0) {
        timeout.QuadPart = -10LL * 1000LL * (LONGLONG)waitRequest->TimeoutMs;
        timeoutPtr = &timeout;
    }

    status = KeWaitForMultipleObjects(
        2,
        waitObjects,
        WaitAny,
        Executive,
        KernelMode,
        FALSE,
        timeoutPtr,
        waitBlocks);
    UNREFERENCED_PARAMETER(status);

    if (FileEventGetQueueLength() > 0) {
        waitRequest->ReadyMask |= PS_EVENT_FILE_READY;
    }
    if (NetEventGetQueueLength() > 0) {
        waitRequest->ReadyMask |= PS_EVENT_NET_READY;
    }

    Irp->IoStatus.Information = sizeof(*waitRequest);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleSetProtectListIrp(_In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    Irp->IoStatus.Information = 0;
    if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(PS_PROTECT_LIST)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    return ProtectSetList((PPS_PROTECT_LIST)Irp->AssociatedIrp.SystemBuffer);
}

NTSTATUS
HandleGetProtectChallengeIrp(_In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    Irp->IoStatus.Information = 0;
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS_PROTECT_TICKET)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    status = ProtectGetChallenge((PPS_PROTECT_TICKET)Irp->AssociatedIrp.SystemBuffer);
    if (NT_SUCCESS(status)) Irp->IoStatus.Information = sizeof(PS_PROTECT_TICKET);
    return status;
}

NTSTATUS
HandleUnlockProtectionIrp(_In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    Irp->IoStatus.Information = 0;
    if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(PS_PROTECT_UNLOCK_REQUEST)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    return ProtectUnlock((PPS_PROTECT_UNLOCK_REQUEST)Irp->AssociatedIrp.SystemBuffer);
}

NTSTATUS
HandleQueryProtectStateIrp(_In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    Irp->IoStatus.Information = 0;
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS_PROTECT_STATUS)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    ProtectQueryState((PPS_PROTECT_STATUS)Irp->AssociatedIrp.SystemBuffer);
    Irp->IoStatus.Information = sizeof(PS_PROTECT_STATUS);
    return STATUS_SUCCESS;
}

NTSTATUS
HandleProtectControlIrp(_In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    Irp->IoStatus.Information = 0;
    if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(PS_PROTECT_CONTROL)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    return ProtectControl((PPS_PROTECT_CONTROL)Irp->AssociatedIrp.SystemBuffer);
}
