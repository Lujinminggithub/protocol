/*
 * wfp_callout.c - WFP callout registration
 */

#include "wfp_callout.h"
#include "ale_auth.h"
#include "classification.h"
#include "../device.h"
#include "../common/shared_events.h"

DEFINE_GUID(PROVIDER_GUID,
    0xfa640d18, 0x9bc2, 0x443f,
    0x8f, 0xd3, 0xab, 0x94, 0x39, 0xd0, 0xd4, 0xb9);

DEFINE_GUID(SUB_LAYER_GUID,
    0x9a5108d2, 0x7e19, 0x4c3b,
    0xb2, 0xf4, 0xa1, 0xc3, 0xd5, 0xe7, 0xf9, 0x0b);

DEFINE_GUID(OUTBOUND_CALLOUT_V4_GUID,
    0x1a2b3c4d, 0x5e6f, 0x7a8b,
    0x9c, 0x0d, 0x1e, 0x2f, 0x3a, 0x4b, 0x5c, 0x6d);

DEFINE_GUID(INBOUND_CALLOUT_V4_GUID,
    0x2b3c4d5e, 0x6f7a, 0x8b9c,
    0x0d, 0x1e, 0x2f, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e);

DEFINE_GUID(OUTBOUND_CALLOUT_V6_GUID,
    0x3c4d5e6f, 0x7a8b, 0x9cad,
    0x1e, 0x2f, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f);

DEFINE_GUID(INBOUND_CALLOUT_V6_GUID,
    0x4d5e6f7a, 0x8b9c, 0xadb0,
    0x2f, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90);

DEFINE_GUID(STREAM_CALLOUT_V4_GUID,
    0x5e6f7a8b, 0x9cad, 0xb0c1,
    0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90, 0xa1);

DEFINE_GUID(STREAM_CALLOUT_V6_GUID,
    0x6f7a8b9c, 0xadb0, 0xc1d2,
    0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90, 0xa1, 0xb2);

DEFINE_GUID(REDIRECT_CALLOUT_V4_GUID,
    0x7a8b9cad, 0xb0c1, 0xd2e3,
    0x5c, 0x6d, 0x7e, 0x8f, 0x90, 0xa1, 0xb2, 0xc3);

DEFINE_GUID(REDIRECT_CALLOUT_V6_GUID,
    0x8b9cadb0, 0xc1d2, 0xe3f4,
    0x6d, 0x7e, 0x8f, 0x90, 0xa1, 0xb2, 0xc3, 0xd4);

DEFINE_GUID(FLOW_CALLOUT_V4_GUID,
    0x9cadb0c1, 0xd2e3, 0xf405,
    0x7e, 0x8f, 0x90, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5);

DEFINE_GUID(FLOW_CALLOUT_V6_GUID,
    0xadb0c1d2, 0xe3f4, 0x0516,
    0x8f, 0x90, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6);

typedef struct _PS_REDIRECT_ENTRY {
    LIST_ENTRY ListEntry;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UpdateTime;
    ULONG ProcessId;
    UINT8 Protocol;
    UINT8 AddressFamily;
    UINT16 LocalPort;
    UCHAR LocalAddress[PS_REDIRECT_ADDR_MAX];
    UINT16 OriginalRemotePort;
    UCHAR OriginalRemoteAddress[PS_REDIRECT_ADDR_MAX];
    UINT64 FlowId;
    UINT16 LayerId;
    BOOLEAN FlowBound;
} PS_REDIRECT_ENTRY, *PPS_REDIRECT_ENTRY;

static HANDLE gEngineHandle = NULL;
static UINT32 gOutboundCalloutV4Id = 0;
static UINT32 gInboundCalloutV4Id = 0;
static UINT32 gOutboundCalloutV6Id = 0;
static UINT32 gInboundCalloutV6Id = 0;
static UINT32 gStreamCalloutV4Id = 0;
static UINT32 gStreamCalloutV6Id = 0;
static UINT32 gRedirectCalloutV4Id = 0;
static UINT32 gRedirectCalloutV6Id = 0;
static UINT32 gFlowCalloutV4Id = 0;
static UINT32 gFlowCalloutV6Id = 0;
static BOOLEAN gOutboundV4Registered = FALSE;
static BOOLEAN gInboundV4Registered = FALSE;
static BOOLEAN gOutboundV6Registered = FALSE;
static BOOLEAN gInboundV6Registered = FALSE;
static BOOLEAN gStreamV4Registered = FALSE;
static BOOLEAN gStreamV6Registered = FALSE;
static BOOLEAN gRedirectV4Registered = FALSE;
static BOOLEAN gRedirectV6Registered = FALSE;
static BOOLEAN gFlowV4Registered = FALSE;
static BOOLEAN gFlowV6Registered = FALSE;
static BOOLEAN gWfpInitialized = FALSE;
static NTSTATUS gWfpLastError = STATUS_SUCCESS;
static HANDLE gRedirectHandle = NULL;
static PS_REDIRECT_SETTINGS gRedirectSettings = { 0 };
static LIST_ENTRY gRedirectList;
static KSPIN_LOCK gRedirectLock;
static BOOLEAN gRedirectStateInitialized = FALSE;
static const LONGLONG kRedirectEntryTtl = 5LL * 60LL * 1000LL * 1000LL * 10LL;

static
VOID
RedirectInitializeState(VOID)
{
    if (!gRedirectStateInitialized) {
        InitializeListHead(&gRedirectList);
        KeInitializeSpinLock(&gRedirectLock);
        gRedirectStateInitialized = TRUE;
    }
}

static
USHORT
RedirectAddressLength(
    _In_ UINT8 AddressFamily
    )
{
    return AddressFamily == AF_INET6 ? 16 : 4;
}

static
VOID
RedirectReapExpiredEntriesLocked(
    _In_ BOOLEAN ForceOne
    )
{
    PLIST_ENTRY entry;
    LARGE_INTEGER now;

    if (!gRedirectStateInitialized || IsListEmpty(&gRedirectList)) {
        return;
    }

    KeQuerySystemTime(&now);
    entry = gRedirectList.Flink;
    while (entry != &gRedirectList) {
        PPS_REDIRECT_ENTRY redirectEntry;
        PLIST_ENTRY nextEntry;
        BOOLEAN expired;

        redirectEntry = CONTAINING_RECORD(entry, PS_REDIRECT_ENTRY, ListEntry);
        nextEntry = entry->Flink;
        expired = !redirectEntry->FlowBound &&
            (now.QuadPart - redirectEntry->UpdateTime.QuadPart) >= kRedirectEntryTtl;

        if (expired || ForceOne) {
            RemoveEntryList(&redirectEntry->ListEntry);
            ExFreePool(redirectEntry);
            if (ForceOne) {
                break;
            }
        }

        entry = nextEntry;
    }
}

static
PPS_REDIRECT_ENTRY
RedirectFindByFlowLocked(
    _In_ UINT64 FlowId,
    _In_ UINT16 LayerId
    )
{
    PLIST_ENTRY entry;

    entry = gRedirectList.Flink;
    while (entry != &gRedirectList) {
        PPS_REDIRECT_ENTRY redirectEntry;

        redirectEntry = CONTAINING_RECORD(entry, PS_REDIRECT_ENTRY, ListEntry);
        if (redirectEntry->FlowBound &&
            redirectEntry->FlowId == FlowId &&
            redirectEntry->LayerId == LayerId) {
            return redirectEntry;
        }
        entry = entry->Flink;
    }

    return NULL;
}

static
PPS_REDIRECT_ENTRY
RedirectFindEntryLocked(
    _In_ ULONG ProcessId,
    _In_ UINT8 Protocol,
    _In_ UINT8 AddressFamily,
    _In_ UINT16 LocalPort,
    _In_reads_(RedirectAddressLength(AddressFamily)) const UCHAR* LocalAddress
    )
{
    PLIST_ENTRY entry;
    USHORT addressBytes;

    addressBytes = RedirectAddressLength(AddressFamily);

    entry = gRedirectList.Flink;
    while (entry != &gRedirectList) {
        PPS_REDIRECT_ENTRY redirectEntry;

        redirectEntry = CONTAINING_RECORD(entry, PS_REDIRECT_ENTRY, ListEntry);
        if (redirectEntry->ProcessId == ProcessId &&
            redirectEntry->Protocol == Protocol &&
            redirectEntry->AddressFamily == AddressFamily &&
            redirectEntry->LocalPort == LocalPort &&
            RtlCompareMemory(redirectEntry->LocalAddress, LocalAddress, addressBytes) == addressBytes) {
            return redirectEntry;
        }
        entry = entry->Flink;
    }

    return NULL;
}

static
BOOLEAN
RedirectIsLoopbackV4(
    _In_ UINT32 Address
    )
{
    return ((Address >> 24) & 0xFF) == 127;
}

static
BOOLEAN
RedirectIsLoopbackV6(
    _In_reads_(16) const UCHAR* Address
    )
{
    static const UCHAR loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    return Address != NULL && RtlCompareMemory(Address, loopback, 16) == 16;
}

static NTSTATUS NTAPI
WfpNotifyFn(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE NotifyType,
    _In_ const GUID* FilterKey,
    _In_ const FWPS_FILTER0* Filter
    )
{
    UNREFERENCED_PARAMETER(NotifyType);
    UNREFERENCED_PARAMETER(FilterKey);
    UNREFERENCED_PARAMETER(Filter);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
WfpNotifyFn1(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE NotifyType,
    _In_ const GUID* FilterKey,
    _Inout_ FWPS_FILTER1* Filter
    )
{
    UNREFERENCED_PARAMETER(NotifyType);
    UNREFERENCED_PARAMETER(FilterKey);
    UNREFERENCED_PARAMETER(Filter);
    return STATUS_SUCCESS;
}

BOOLEAN
WfpRedirectIsActive(VOID)
{
    return gRedirectSettings.Enabled != 0 &&
        gRedirectSettings.ProxyProcessId != 0 &&
        gRedirectSettings.ProxyPort != 0 &&
        gRedirectHandle != NULL;
}

HANDLE
WfpRedirectGetHandle(VOID)
{
    return gRedirectHandle;
}

NTSTATUS
WfpRedirectSetSettings(
    _In_ const PS_REDIRECT_SETTINGS* Settings
    )
{
    KIRQL oldIrql;

    if (Settings == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RedirectInitializeState();
    KeAcquireSpinLock(&gRedirectLock, &oldIrql);
    RtlCopyMemory(&gRedirectSettings, Settings, sizeof(gRedirectSettings));
    KeReleaseSpinLock(&gRedirectLock, oldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS
WfpRedirectGetSettings(
    _Out_ PS_REDIRECT_SETTINGS* Settings
    )
{
    KIRQL oldIrql;

    if (Settings == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RedirectInitializeState();
    KeAcquireSpinLock(&gRedirectLock, &oldIrql);
    RtlCopyMemory(Settings, &gRedirectSettings, sizeof(*Settings));
    KeReleaseSpinLock(&gRedirectLock, oldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS
WfpRedirectRecordDestination(
    _In_ ULONG ProcessId,
    _In_ UINT8 Protocol,
    _In_ UINT8 AddressFamily,
    _In_ UINT16 LocalPort,
    _In_reads_(AddressBytes) const UCHAR* LocalAddress,
    _In_ USHORT AddressBytes,
    _In_ UINT16 OriginalRemotePort,
    _In_reads_(AddressBytes) const UCHAR* OriginalRemoteAddress
    )
{
    PPS_REDIRECT_ENTRY entry;
    KIRQL oldIrql;

    if (AddressBytes == 0 || LocalAddress == NULL || OriginalRemoteAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RedirectInitializeState();
    KeAcquireSpinLock(&gRedirectLock, &oldIrql);
    RedirectReapExpiredEntriesLocked(FALSE);
    entry = RedirectFindEntryLocked(ProcessId, Protocol, AddressFamily, LocalPort, LocalAddress);
    if (entry == NULL) {
        entry = (PPS_REDIRECT_ENTRY)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*entry), 'rdPS');
        if (entry == NULL) {
            KeReleaseSpinLock(&gRedirectLock, oldIrql);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(entry, sizeof(*entry));
        InsertTailList(&gRedirectList, &entry->ListEntry);
    }

    KeQuerySystemTime(&entry->CreateTime);
    entry->UpdateTime = entry->CreateTime;
    entry->ProcessId = ProcessId;
    entry->Protocol = Protocol;
    entry->AddressFamily = AddressFamily;
    entry->LocalPort = LocalPort;
    RtlZeroMemory(entry->LocalAddress, sizeof(entry->LocalAddress));
    RtlZeroMemory(entry->OriginalRemoteAddress, sizeof(entry->OriginalRemoteAddress));
    RtlCopyMemory(entry->LocalAddress, LocalAddress, AddressBytes);
    entry->OriginalRemotePort = OriginalRemotePort;
    RtlCopyMemory(entry->OriginalRemoteAddress, OriginalRemoteAddress, AddressBytes);
    entry->FlowBound = FALSE;
    entry->FlowId = 0;
    entry->LayerId = 0;
    KeReleaseSpinLock(&gRedirectLock, oldIrql);

    return STATUS_SUCCESS;
}

NTSTATUS
WfpRedirectBindFlow(
    _In_ ULONG ProcessId,
    _In_ UINT8 Protocol,
    _In_ UINT8 AddressFamily,
    _In_ UINT16 LocalPort,
    _In_reads_(PS_REDIRECT_ADDR_MAX) const UCHAR* LocalAddress,
    _In_ UINT64 FlowId,
    _In_ UINT16 LayerId
    )
{
    PPS_REDIRECT_ENTRY entry;
    KIRQL oldIrql;

    if (LocalAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RedirectInitializeState();
    KeAcquireSpinLock(&gRedirectLock, &oldIrql);
    entry = RedirectFindEntryLocked(ProcessId, Protocol, AddressFamily, LocalPort, LocalAddress);
    if (entry != NULL) {
        entry->FlowBound = TRUE;
        entry->FlowId = FlowId;
        entry->LayerId = LayerId;
        KeQuerySystemTime(&entry->UpdateTime);
    }
    KeReleaseSpinLock(&gRedirectLock, oldIrql);
    return entry != NULL ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

VOID
WfpRedirectFlowDelete(
    _In_ UINT64 FlowId,
    _In_ UINT16 LayerId
    )
{
    PPS_REDIRECT_ENTRY entry;
    KIRQL oldIrql;

    if (!gRedirectStateInitialized) {
        return;
    }

    KeAcquireSpinLock(&gRedirectLock, &oldIrql);
    entry = RedirectFindByFlowLocked(FlowId, LayerId);
    if (entry != NULL) {
        RemoveEntryList(&entry->ListEntry);
        ExFreePool(entry);
    }
    KeReleaseSpinLock(&gRedirectLock, oldIrql);
}

NTSTATUS
WfpRedirectQueryOriginalDestination(
    _Inout_ PS_REDIRECT_QUERY* Query
    )
{
    PPS_REDIRECT_ENTRY entry;
    KIRQL oldIrql;
    USHORT addressBytes;

    if (Query == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RedirectInitializeState();
    addressBytes = RedirectAddressLength(Query->AddressFamily);
    Query->Found = 0;

    KeAcquireSpinLock(&gRedirectLock, &oldIrql);
    RedirectReapExpiredEntriesLocked(FALSE);
    entry = RedirectFindEntryLocked(
        Query->ProcessId,
        Query->Protocol,
        Query->AddressFamily,
        Query->LocalPort,
        Query->LocalAddress);
    if (entry != NULL) {
        Query->Found = 1;
        Query->OriginalRemotePort = entry->OriginalRemotePort;
        RtlZeroMemory(Query->OriginalRemoteAddress, sizeof(Query->OriginalRemoteAddress));
        RtlCopyMemory(Query->OriginalRemoteAddress, entry->OriginalRemoteAddress, addressBytes);
    }
    KeReleaseSpinLock(&gRedirectLock, oldIrql);

    return STATUS_SUCCESS;
}

static
NTSTATUS
RegisterKernelCallout(
    _In_ const GUID* CalloutKey,
    _In_ FWPS_CALLOUT_CLASSIFY_FN0 ClassifyFn,
    _In_opt_ FWPS_CALLOUT_FLOW_DELETE_NOTIFY_FN0 FlowDeleteFn,
    _Out_ UINT32* OutId
    )
{
    FWPS_CALLOUT0 callout;

    RtlZeroMemory(&callout, sizeof(callout));
    callout.calloutKey = *CalloutKey;
    callout.classifyFn = ClassifyFn;
    callout.notifyFn = WfpNotifyFn;
    callout.flowDeleteFn = FlowDeleteFn;

    return FwpsCalloutRegister0(gDeviceObject, &callout, OutId);
}

static
NTSTATUS
RegisterKernelCallout1(
    _In_ const GUID* CalloutKey,
    _In_ FWPS_CALLOUT_CLASSIFY_FN1 ClassifyFn,
    _Out_ UINT32* OutId
    )
{
    FWPS_CALLOUT1 callout;

    RtlZeroMemory(&callout, sizeof(callout));
    callout.calloutKey = *CalloutKey;
    callout.classifyFn = ClassifyFn;
    callout.notifyFn = WfpNotifyFn1;

    return FwpsCalloutRegister1(gDeviceObject, &callout, OutId);
}

static
NTSTATUS
AddCalloutAndFilter(
    _In_ const GUID* CalloutKey,
    _In_ const GUID* LayerKey,
    _In_ PCWSTR Name,
    _In_ FWP_ACTION_TYPE ActionType
    )
{
    FWPM_CALLOUT0 managementCallout;
    FWPM_FILTER0 filter;
    NTSTATUS status;

    RtlZeroMemory(&managementCallout, sizeof(managementCallout));
    managementCallout.calloutKey = *CalloutKey;
    managementCallout.displayData.name = (PWSTR)Name;
    managementCallout.applicableLayer = *LayerKey;
    managementCallout.providerKey = (GUID*)&PROVIDER_GUID;

    status = FwpmCalloutAdd0(gEngineHandle, &managementCallout, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&filter, sizeof(filter));
    filter.layerKey = *LayerKey;
    filter.displayData.name = (PWSTR)Name;
    filter.providerKey = (GUID*)&PROVIDER_GUID;
    filter.action.type = ActionType;
    filter.action.calloutKey = *CalloutKey;
    filter.subLayerKey = SUB_LAYER_GUID;
    filter.weight.type = FWP_EMPTY;

    return FwpmFilterAdd0(gEngineHandle, &filter, NULL, NULL);
}

BOOLEAN
WfpCalloutIsInitialized(VOID)
{
    return gWfpInitialized;
}

NTSTATUS
WfpCalloutGetLastError(VOID)
{
    return gWfpLastError;
}

NTSTATUS
WfpCalloutInitialize(VOID)
{
    FWPM_SESSION0 session;
    FWPM_PROVIDER0 provider;
    FWPM_SUBLAYER0 subLayer;
    NTSTATUS status;

    if (gWfpInitialized) {
        return STATUS_SUCCESS;
    }

    RedirectInitializeState();

    if (gDeviceObject == NULL) {
        gWfpLastError = STATUS_DEVICE_NOT_READY;
        return gWfpLastError;
    }

    status = RegisterKernelCallout(&OUTBOUND_CALLOUT_V4_GUID, AleAuthNotifyOutboundV4, NULL, &gOutboundCalloutV4Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (outbound v4) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gOutboundV4Registered = TRUE;

    status = RegisterKernelCallout(&INBOUND_CALLOUT_V4_GUID, AleAuthNotifyInboundV4, NULL, &gInboundCalloutV4Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (inbound v4) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gInboundV4Registered = TRUE;

    status = RegisterKernelCallout(&OUTBOUND_CALLOUT_V6_GUID, AleAuthNotifyOutboundV6, NULL, &gOutboundCalloutV6Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (outbound v6) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gOutboundV6Registered = TRUE;

    status = RegisterKernelCallout(&INBOUND_CALLOUT_V6_GUID, AleAuthNotifyInboundV6, NULL, &gInboundCalloutV6Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (inbound v6) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gInboundV6Registered = TRUE;

    RtlZeroMemory(&session, sizeof(session));
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    status = FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, &session, &gEngineHandle);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpmEngineOpen0 failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }

    status = FwpmTransactionBegin0(gEngineHandle, 0);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        WfpCalloutCleanup();
        return status;
    }

    RtlZeroMemory(&provider, sizeof(provider));
    provider.providerKey = PROVIDER_GUID;
    provider.displayData.name = L"PersonalSafer WFP Provider";
    provider.displayData.description = L"PersonalSafer network inspection provider";
    provider.serviceName = L"PersonalSafer";

    status = FwpmProviderAdd0(gEngineHandle, &provider, NULL);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpmProviderAdd0 failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    RtlZeroMemory(&subLayer, sizeof(subLayer));
    subLayer.subLayerKey = SUB_LAYER_GUID;
    subLayer.displayData.name = L"PersonalSafer SubLayer";
    subLayer.providerKey = (GUID*)&PROVIDER_GUID;
    subLayer.weight = 0x8000;

    status = FwpmSubLayerAdd0(gEngineHandle, &subLayer, NULL);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&OUTBOUND_CALLOUT_V4_GUID, &FWPM_LAYER_ALE_AUTH_CONNECT_V4, L"PersonalSafer Outbound Connect V4", FWP_ACTION_CALLOUT_TERMINATING);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (outbound v4) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&INBOUND_CALLOUT_V4_GUID, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, L"PersonalSafer Inbound Accept V4", FWP_ACTION_CALLOUT_TERMINATING);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (inbound v4) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&OUTBOUND_CALLOUT_V6_GUID, &FWPM_LAYER_ALE_AUTH_CONNECT_V6, L"PersonalSafer Outbound Connect V6", FWP_ACTION_CALLOUT_TERMINATING);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (outbound v6) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&INBOUND_CALLOUT_V6_GUID, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, L"PersonalSafer Inbound Accept V6", FWP_ACTION_CALLOUT_TERMINATING);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (inbound v6) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = RegisterKernelCallout(&STREAM_CALLOUT_V4_GUID, StreamInspectOutboundV4, NULL, &gStreamCalloutV4Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (stream v4) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gStreamV4Registered = TRUE;

    status = RegisterKernelCallout(&STREAM_CALLOUT_V6_GUID, StreamInspectOutboundV6, NULL, &gStreamCalloutV6Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (stream v6) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gStreamV6Registered = TRUE;

    status = RegisterKernelCallout1(&REDIRECT_CALLOUT_V4_GUID, AleRedirectConnectV4, &gRedirectCalloutV4Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (redirect v4) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gRedirectV4Registered = TRUE;

    status = RegisterKernelCallout1(&REDIRECT_CALLOUT_V6_GUID, AleRedirectConnectV6, &gRedirectCalloutV6Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (redirect v6) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gRedirectV6Registered = TRUE;

    status = RegisterKernelCallout(&FLOW_CALLOUT_V4_GUID, AleFlowEstablishedV4, AleFlowDeleteNotify, &gFlowCalloutV4Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (flow v4) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gFlowV4Registered = TRUE;

    status = RegisterKernelCallout(&FLOW_CALLOUT_V6_GUID, AleFlowEstablishedV6, AleFlowDeleteNotify, &gFlowCalloutV6Id);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsCalloutRegister0 (flow v6) failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }
    gFlowV6Registered = TRUE;

    status = AddCalloutAndFilter(&STREAM_CALLOUT_V4_GUID, &FWPM_LAYER_STREAM_V4, L"PersonalSafer Stream Inspect V4", FWP_ACTION_CALLOUT_UNKNOWN);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (stream v4) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&STREAM_CALLOUT_V6_GUID, &FWPM_LAYER_STREAM_V6, L"PersonalSafer Stream Inspect V6", FWP_ACTION_CALLOUT_UNKNOWN);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (stream v6) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&REDIRECT_CALLOUT_V4_GUID, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V4, L"PersonalSafer Redirect V4", FWP_ACTION_CALLOUT_INSPECTION);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (redirect v4) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&REDIRECT_CALLOUT_V6_GUID, &FWPM_LAYER_ALE_CONNECT_REDIRECT_V6, L"PersonalSafer Redirect V6", FWP_ACTION_CALLOUT_INSPECTION);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (redirect v6) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&FLOW_CALLOUT_V4_GUID, &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4, L"PersonalSafer Flow Established V4", FWP_ACTION_CALLOUT_INSPECTION);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (flow v4) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = AddCalloutAndFilter(&FLOW_CALLOUT_V6_GUID, &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6, L"PersonalSafer Flow Established V6", FWP_ACTION_CALLOUT_INSPECTION);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "AddCalloutAndFilter (flow v6) failed: 0x%x", status);
        FwpmTransactionAbort0(gEngineHandle);
        WfpCalloutCleanup();
        return status;
    }

    status = FwpmTransactionCommit0(gEngineHandle);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpmTransactionCommit0 failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }

    status = FwpsRedirectHandleCreate0(&PROVIDER_GUID, 0, &gRedirectHandle);
    if (!NT_SUCCESS(status)) {
        gWfpLastError = status;
        DLP_LOG(DLP_DEBUG_ERROR, "FwpsRedirectHandleCreate0 failed: 0x%x", status);
        WfpCalloutCleanup();
        return status;
    }

    gWfpInitialized = TRUE;
    gWfpLastError = STATUS_SUCCESS;
    DLP_LOG(DLP_DEBUG_INFO, "WFP callouts initialized");
    return STATUS_SUCCESS;
}

VOID
WfpCalloutCleanup(VOID)
{
    if (gEngineHandle != NULL) {
        FwpmEngineClose0(gEngineHandle);
        gEngineHandle = NULL;
    }

    if (gInboundV6Registered) {
        FwpsCalloutUnregisterById0(gInboundCalloutV6Id);
        gInboundV6Registered = FALSE;
    }

    if (gFlowV6Registered) {
        FwpsCalloutUnregisterById0(gFlowCalloutV6Id);
        gFlowV6Registered = FALSE;
    }

    if (gRedirectV6Registered) {
        FwpsCalloutUnregisterById0(gRedirectCalloutV6Id);
        gRedirectV6Registered = FALSE;
    }

    if (gStreamV6Registered) {
        FwpsCalloutUnregisterById0(gStreamCalloutV6Id);
        gStreamV6Registered = FALSE;
    }

    if (gOutboundV6Registered) {
        FwpsCalloutUnregisterById0(gOutboundCalloutV6Id);
        gOutboundV6Registered = FALSE;
    }

    if (gInboundV4Registered) {
        FwpsCalloutUnregisterById0(gInboundCalloutV4Id);
        gInboundV4Registered = FALSE;
    }

    if (gFlowV4Registered) {
        FwpsCalloutUnregisterById0(gFlowCalloutV4Id);
        gFlowV4Registered = FALSE;
    }

    if (gRedirectV4Registered) {
        FwpsCalloutUnregisterById0(gRedirectCalloutV4Id);
        gRedirectV4Registered = FALSE;
    }

    if (gStreamV4Registered) {
        FwpsCalloutUnregisterById0(gStreamCalloutV4Id);
        gStreamV4Registered = FALSE;
    }

    if (gOutboundV4Registered) {
        FwpsCalloutUnregisterById0(gOutboundCalloutV4Id);
        gOutboundV4Registered = FALSE;
    }

    if (gRedirectHandle != NULL) {
        FwpsRedirectHandleDestroy0(gRedirectHandle);
        gRedirectHandle = NULL;
    }

    if (gRedirectStateInitialized) {
        KIRQL oldIrql;
        KeAcquireSpinLock(&gRedirectLock, &oldIrql);
        while (!IsListEmpty(&gRedirectList)) {
            PLIST_ENTRY entry = RemoveHeadList(&gRedirectList);
            PPS_REDIRECT_ENTRY redirectEntry = CONTAINING_RECORD(entry, PS_REDIRECT_ENTRY, ListEntry);
            ExFreePool(redirectEntry);
        }
        RtlZeroMemory(&gRedirectSettings, sizeof(gRedirectSettings));
        KeReleaseSpinLock(&gRedirectLock, oldIrql);
    }

    gWfpInitialized = FALSE;
    DLP_LOG(DLP_DEBUG_INFO, "WFP callouts cleaned up");
}

NTSTATUS
WfpCalloutRegister(
    _In_ WFP_INJECTION_POINT InjectionPoint,
    _In_ GUID *CalloutGuid
    )
{
    UNREFERENCED_PARAMETER(InjectionPoint);
    UNREFERENCED_PARAMETER(CalloutGuid);
    return STATUS_SUCCESS;
}

NTSTATUS
WfpCalloutUnregister(
    _In_ GUID *CalloutGuid
    )
{
    UNREFERENCED_PARAMETER(CalloutGuid);
    return STATUS_SUCCESS;
}

NTSTATUS
WfpCalloutAddFilter(
    _In_ GUID *CalloutGuid,
    _In_ WFP_INJECTION_POINT Layer,
    _In_ UINT16 RemotePort,
    _In_ BOOLEAN Allow,
    _Out_ UINT64 *FilterId
    )
{
    UNREFERENCED_PARAMETER(CalloutGuid);
    UNREFERENCED_PARAMETER(Layer);
    UNREFERENCED_PARAMETER(RemotePort);
    UNREFERENCED_PARAMETER(Allow);

    if (FilterId != NULL) {
        *FilterId = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
WfpCalloutRemoveFilter(
    _In_ UINT64 FilterId
    )
{
    UNREFERENCED_PARAMETER(FilterId);
    return STATUS_SUCCESS;
}
