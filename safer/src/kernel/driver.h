/*
 * driver.h - PersonalSafer driver globals
 */

#pragma once

#include <fltKernel.h>

#define PS_ALTITUDE L"379950"

extern PFLT_FILTER gFilterHandle;
extern BOOLEAN gDriverStarted;
extern BOOLEAN gDriverAcceptingIo;
extern LARGE_INTEGER gDriverStartTime;
extern IO_REMOVE_LOCK gDeviceRemoveLock;

extern CONST FLT_REGISTRATION gFilterRegistration;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
);

NTSTATUS
PsFilterUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
);
