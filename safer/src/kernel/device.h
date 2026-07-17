/*
 * device.h - 设备创建/IRP 处理声明
 */

#pragma once

#include <fltKernel.h>

// 设备对象
extern PDEVICE_OBJECT gDeviceObject;

// 符号链接名称
#define PS_DEVICE_NAME    L"\\Device\\PersonalSafer"
#define PS_SYMLINK_NAME   L"\\DosDevices\\PersonalSafer"

// 函数声明
NTSTATUS
DeviceCreate(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
);

VOID
DeviceCleanup(VOID);

VOID
DeviceBeginUnload(VOID);

NTSTATUS
DeviceDispatchCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
);

NTSTATUS
DeviceDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
);

NTSTATUS
DeviceDispatchCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
);

NTSTATUS
DeviceDispatchClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
);

NTSTATUS
DeviceDispatchUnhandled(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
);

// IOCTL 处理函数
NTSTATUS
HandleDriverStatusIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleSetPolicyIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleGetPolicyIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleSetQuarantineIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleGetQuarantineIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleSetRedirectIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleQueryRedirectIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleFileEventIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleFileEventBatchIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleNetEventIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleNetEventBatchIrp(
    _In_ PIRP Irp
);

NTSTATUS
HandleWaitEventsIrp(
    _In_ PIRP Irp
);

NTSTATUS HandleSetProtectListIrp(_In_ PIRP Irp);
NTSTATUS HandleGetProtectChallengeIrp(_In_ PIRP Irp);
NTSTATUS HandleUnlockProtectionIrp(_In_ PIRP Irp);
NTSTATUS HandleQueryProtectStateIrp(_In_ PIRP Irp);
NTSTATUS HandleProtectControlIrp(_In_ PIRP Irp);
