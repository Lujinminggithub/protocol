/*
 * ale_auth.c - ALE auth WFP classify callbacks
 */

#include "ale_auth.h"
#include "classification.h"
#include "net_event.h"
#include "wfp_callout.h"
#include "../filter/process_tracker.h"
#include "../policy/policy_engine.h"
#include "../common/shared_events.h"
#include <ntstrsafe.h>

static
USHORT
FormatIpv4(
    _In_ UINT32 Address,
    _Out_writes_bytes_(BufferSize) UCHAR* Buffer,
    _In_ SIZE_T BufferSize
    )
{
    NTSTATUS status;

    status = RtlStringCbPrintfA(
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
RedirectAddressBytes(
    _In_ UINT8 AddressFamily
    )
{
    return AddressFamily == AF_INET6 ? 16 : 4;
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
VOID
FinalizeNetEvent(
    _Inout_ PDLP_NET_EVENT Event,
    _In_ ULONG ProcessId,
    _In_ DLP_EVENT_TYPE EventType,
    _In_ UINT8 Protocol,
    _In_ UINT16 RemotePort,
    _In_ UINT16 LocalPort
    )
{
    ACTION_RESULT action;

    Event->EventType = EventType;
    Event->ProcessId = ProcessId;
    Event->Protocol = Protocol;
    Event->RemotePort = RemotePort;
    Event->LocalPort = LocalPort;
    KeQuerySystemTime(&Event->Timestamp);
    Event->ProcessNameLength = 0;
    if (KeGetCurrentIrql() <= APC_LEVEL) {
        Event->ProcessNameLength = ProcessTrackerGetProcessNameById(
            (HANDLE)(ULONG_PTR)ProcessId,
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

    action = PolicyEngineQueryNetAction(ProcessId, RemotePort, NULL, EventType);
    Event->ActionResult = action;
}

static
BOOLEAN
TryClassifyFtpDataChannelV4(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut,
    _In_ DLP_EVENT_TYPE EventType
    )
{
    PDLP_NET_EVENT event;
    ACTION_RESULT action;

    event = NetEventAllocate();
    if (event == NULL) {
        return FALSE;
    }
    if (!ClassificationTryMatchFtpDataChannelV4(InFixedValues, InMetaValues, EventType, event)) {
        NetEventFree(event);
        return FALSE;
    }

    action = event->ActionResult;
    NetEventEnqueue(event);
    NetEventFree(event);
    if (action == ActionBlocked) {
        ClassifyOut->actionType = FWP_ACTION_BLOCK;
        ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
        ClassifyOut->flags |= FWPS_CLASSIFY_OUT_FLAG_ABSORB;
    } else {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
    }
    return TRUE;
}

static
BOOLEAN
TryClassifyFtpDataChannelV6(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut,
    _In_ DLP_EVENT_TYPE EventType
    )
{
    PDLP_NET_EVENT event;
    ACTION_RESULT action;

    event = NetEventAllocate();
    if (event == NULL) {
        return FALSE;
    }
    if (!ClassificationTryMatchFtpDataChannelV6(InFixedValues, InMetaValues, EventType, event)) {
        NetEventFree(event);
        return FALSE;
    }

    action = event->ActionResult;
    NetEventEnqueue(event);
    NetEventFree(event);
    if (action == ActionBlocked) {
        ClassifyOut->actionType = FWP_ACTION_BLOCK;
        ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
        ClassifyOut->flags |= FWPS_CLASSIFY_OUT_FLAG_ABSORB;
    } else {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
    }
    return TRUE;
}

static
VOID
ClassifyConnectionV4(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut,
    _In_ DLP_EVENT_TYPE EventType,
    _In_ UINT32 IdxRemoteAddr,
    _In_ UINT32 IdxRemotePort,
    _In_ UINT32 IdxLocalAddr,
    _In_ UINT32 IdxLocalPort,
    _In_ UINT32 IdxProtocol
    )
{
    UINT32 remoteAddr = 0;
    UINT32 localAddr = 0;
    UINT16 remotePort = 0;
    UINT16 localPort = 0;
    UINT8 protocol = 0;
    ULONG processId = 0;
    PDLP_NET_EVENT event;
    ACTION_RESULT action;

    if (ClassifyOut == NULL || (ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) == 0) {
        return;
    }

    if (InFixedValues != NULL) {
        remoteAddr = InFixedValues->incomingValue[IdxRemoteAddr].value.uint32;
        remotePort = InFixedValues->incomingValue[IdxRemotePort].value.uint16;
        localAddr = InFixedValues->incomingValue[IdxLocalAddr].value.uint32;
        localPort = InFixedValues->incomingValue[IdxLocalPort].value.uint16;
        protocol = InFixedValues->incomingValue[IdxProtocol].value.uint8;
    }

    if (InMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_ID)) {
        processId = (ULONG)InMetaValues->processId;
    }

    if (TryClassifyFtpDataChannelV4(InFixedValues, InMetaValues, ClassifyOut, EventType)) {
        return;
    }

    event = NetEventAllocate();
    if (event == NULL) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }
    event->RemoteAddressLength = FormatIpv4(remoteAddr, event->RemoteAddress, sizeof(event->RemoteAddress));
    event->LocalAddressLength = FormatIpv4(localAddr, event->LocalAddress, sizeof(event->LocalAddress));
    FinalizeNetEvent(event, processId, EventType, protocol, remotePort, localPort);
    action = event->ActionResult;
    NetEventEnqueue(event);
    NetEventFree(event);

    if (action == ActionBlocked) {
        ClassifyOut->actionType = FWP_ACTION_BLOCK;
        ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
        ClassifyOut->flags |= FWPS_CLASSIFY_OUT_FLAG_ABSORB;
    } else {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
    }
}

static
VOID
ClassifyConnectionV6(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut,
    _In_ DLP_EVENT_TYPE EventType,
    _In_ UINT32 IdxRemoteAddr,
    _In_ UINT32 IdxRemotePort,
    _In_ UINT32 IdxLocalAddr,
    _In_ UINT32 IdxLocalPort,
    _In_ UINT32 IdxProtocol
    )
{
    const FWP_BYTE_ARRAY16* remoteAddr = NULL;
    const FWP_BYTE_ARRAY16* localAddr = NULL;
    UINT16 remotePort = 0;
    UINT16 localPort = 0;
    UINT8 protocol = 0;
    ULONG processId = 0;
    PDLP_NET_EVENT event;
    ACTION_RESULT action;

    if (ClassifyOut == NULL || (ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) == 0) {
        return;
    }

    if (InFixedValues != NULL) {
        remoteAddr = InFixedValues->incomingValue[IdxRemoteAddr].value.byteArray16;
        remotePort = InFixedValues->incomingValue[IdxRemotePort].value.uint16;
        localAddr = InFixedValues->incomingValue[IdxLocalAddr].value.byteArray16;
        localPort = InFixedValues->incomingValue[IdxLocalPort].value.uint16;
        protocol = InFixedValues->incomingValue[IdxProtocol].value.uint8;
    }

    if (InMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_ID)) {
        processId = (ULONG)InMetaValues->processId;
    }

    if (TryClassifyFtpDataChannelV6(InFixedValues, InMetaValues, ClassifyOut, EventType)) {
        return;
    }

    event = NetEventAllocate();
    if (event == NULL) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }
    event->RemoteAddressLength = FormatIpv6(remoteAddr, event->RemoteAddress, sizeof(event->RemoteAddress));
    event->LocalAddressLength = FormatIpv6(localAddr, event->LocalAddress, sizeof(event->LocalAddress));
    FinalizeNetEvent(event, processId, EventType, protocol, remotePort, localPort);
    action = event->ActionResult;
    NetEventEnqueue(event);
    NetEventFree(event);

    if (action == ActionBlocked) {
        ClassifyOut->actionType = FWP_ACTION_BLOCK;
        ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
        ClassifyOut->flags |= FWPS_CLASSIFY_OUT_FLAG_ABSORB;
    } else {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
    }
}

VOID NTAPI
AleAuthNotifyOutboundV4(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_ const FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(LayerData);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(FlowContext);

    ClassifyConnectionV4(
        InFixedValues,
        InMetaValues,
        ClassifyOut,
        EventNetworkConnect,
        FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS,
        FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_PORT,
        FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_ADDRESS,
        FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_PORT,
        FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_PROTOCOL);
}

VOID NTAPI
AleAuthNotifyInboundV4(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_ const FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(LayerData);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(FlowContext);

    ClassifyConnectionV4(
        InFixedValues,
        InMetaValues,
        ClassifyOut,
        EventNetworkConnect,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_REMOTE_ADDRESS,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_REMOTE_PORT,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_LOCAL_ADDRESS,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_LOCAL_PORT,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_PROTOCOL);
}

VOID NTAPI
AleAuthNotifyOutboundV6(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_ const FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(LayerData);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(FlowContext);

    ClassifyConnectionV6(
        InFixedValues,
        InMetaValues,
        ClassifyOut,
        EventNetworkConnect,
        FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_REMOTE_ADDRESS,
        FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_REMOTE_PORT,
        FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_LOCAL_ADDRESS,
        FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_LOCAL_PORT,
        FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_PROTOCOL);
}

VOID NTAPI
AleAuthNotifyInboundV6(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_ const FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(LayerData);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(FlowContext);

    ClassifyConnectionV6(
        InFixedValues,
        InMetaValues,
        ClassifyOut,
        EventNetworkConnect,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_REMOTE_ADDRESS,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_REMOTE_PORT,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_LOCAL_ADDRESS,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_LOCAL_PORT,
        FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_IP_PROTOCOL);
}

static
BOOLEAN
RedirectPopulateProxyEndpoint(
    _In_ const PS_REDIRECT_SETTINGS* Settings,
    _In_ UINT8 AddressFamily,
    _Out_ SOCKADDR_STORAGE* Address
    )
{
    if (Settings == NULL || Address == NULL || Settings->AddressFamily != AddressFamily) {
        return FALSE;
    }

    RtlZeroMemory(Address, sizeof(*Address));
    if (AddressFamily == AF_INET) {
        SOCKADDR_IN* ipv4 = (SOCKADDR_IN*)Address;
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = RtlUshortByteSwap(Settings->ProxyPort);
        RtlCopyMemory(&ipv4->sin_addr, Settings->ProxyAddress, sizeof(ipv4->sin_addr));
        return TRUE;
    }

    if (AddressFamily == AF_INET6) {
        SOCKADDR_IN6* ipv6 = (SOCKADDR_IN6*)Address;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = RtlUshortByteSwap(Settings->ProxyPort);
        RtlCopyMemory(&ipv6->sin6_addr, Settings->ProxyAddress, sizeof(ipv6->sin6_addr));
        return TRUE;
    }

    return FALSE;
}

static
VOID
RedirectConnectCommon(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_opt_ const VOID* ClassifyContext,
    _In_ const FWPS_FILTER1* Filter,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut,
    _In_ UINT8 AddressFamily,
    _In_ UINT32 LocalAddrIndex,
    _In_ UINT32 LocalPortIndex,
    _In_ UINT32 RemoteAddrIndex,
    _In_ UINT32 RemotePortIndex,
    _In_ UINT32 ProtocolIndex
    )
{
    PS_REDIRECT_SETTINGS settings;
    HANDLE redirectHandle;
    FWPS_CONNECTION_REDIRECT_STATE redirectState;
    SOCKADDR_STORAGE proxyAddress;
    FWPS_CONNECT_REQUEST0* connectRequest = NULL;
    UINT64 classifyHandle = 0;
    NTSTATUS status;
    ULONG processId = 0;
    UINT16 localPort = 0;
    UINT16 remotePort = 0;
    UINT8 protocol = 0;
    USHORT addressBytes = RedirectAddressBytes(AddressFamily);
    UCHAR localAddress[PS_REDIRECT_ADDR_MAX] = { 0 };
    UCHAR remoteAddress[PS_REDIRECT_ADDR_MAX] = { 0 };

    if (ClassifyOut == NULL || (ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) == 0) {
        return;
    }
    if (LayerData == NULL || Filter == NULL || InFixedValues == NULL) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    status = WfpRedirectGetSettings(&settings);
    if (!NT_SUCCESS(status) || settings.Enabled == 0 || settings.ProxyProcessId == 0 || settings.ProxyPort == 0) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    if (InMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_ID)) {
        processId = (ULONG)InMetaValues->processId;
    }
    if (processId == 0 || processId == settings.ProxyProcessId) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    protocol = InFixedValues->incomingValue[ProtocolIndex].value.uint8;
    if (protocol != IPPROTO_TCP) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    redirectHandle = WfpRedirectGetHandle();
    if (redirectHandle == NULL) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    if (InMetaValues != NULL &&
        FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_REDIRECT_RECORD_HANDLE)) {
        redirectState = FwpsQueryConnectionRedirectState0(
            InMetaValues->redirectRecords,
            redirectHandle,
            NULL);
        if (redirectState != FWPS_CONNECTION_NOT_REDIRECTED) {
            ClassifyOut->actionType = FWP_ACTION_PERMIT;
            return;
        }
    }

    if (!RedirectPopulateProxyEndpoint(&settings, AddressFamily, &proxyAddress)) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    if (AddressFamily == AF_INET) {
        UINT32 localV4 = InFixedValues->incomingValue[LocalAddrIndex].value.uint32;
        UINT32 remoteV4 = InFixedValues->incomingValue[RemoteAddrIndex].value.uint32;
        RtlCopyMemory(localAddress, &localV4, sizeof(localV4));
        RtlCopyMemory(remoteAddress, &remoteV4, sizeof(remoteV4));
    } else {
        const FWP_BYTE_ARRAY16* localV6 = InFixedValues->incomingValue[LocalAddrIndex].value.byteArray16;
        const FWP_BYTE_ARRAY16* remoteV6 = InFixedValues->incomingValue[RemoteAddrIndex].value.byteArray16;
        if (localV6 == NULL || remoteV6 == NULL) {
            ClassifyOut->actionType = FWP_ACTION_PERMIT;
            return;
        }
        RtlCopyMemory(localAddress, localV6->byteArray16, 16);
        RtlCopyMemory(remoteAddress, remoteV6->byteArray16, 16);
    }

    localPort = InFixedValues->incomingValue[LocalPortIndex].value.uint16;
    remotePort = InFixedValues->incomingValue[RemotePortIndex].value.uint16;
    if (remotePort != 80 && remotePort != 443) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    status = FwpsAcquireClassifyHandle0((void*)ClassifyContext, 0, &classifyHandle);
    if (!NT_SUCCESS(status)) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    status = FwpsAcquireWritableLayerDataPointer0(
        classifyHandle,
        Filter->filterId,
        0,
        (PVOID*)&connectRequest,
        ClassifyOut);
    if (!NT_SUCCESS(status) || connectRequest == NULL) {
        FwpsReleaseClassifyHandle0(classifyHandle);
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    status = WfpRedirectRecordDestination(
        processId,
        protocol,
        AddressFamily,
        localPort,
        localAddress,
        addressBytes,
        remotePort,
        remoteAddress);
    if (!NT_SUCCESS(status)) {
        FwpsReleaseClassifyHandle0(classifyHandle);
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    connectRequest->localRedirectTargetPID = settings.ProxyProcessId;
    connectRequest->localRedirectHandle = redirectHandle;
    RtlCopyMemory(&connectRequest->remoteAddressAndPort, &proxyAddress, sizeof(proxyAddress));
    FwpsApplyModifiedLayerData0(classifyHandle, connectRequest, 0);
    FwpsReleaseClassifyHandle0(classifyHandle);
    ClassifyOut->actionType = FWP_ACTION_PERMIT;
}

VOID NTAPI
AleRedirectConnectV4(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_opt_ const VOID* ClassifyContext,
    _In_ const FWPS_FILTER1* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(FlowContext);

    RedirectConnectCommon(
        InFixedValues,
        InMetaValues,
        LayerData,
        ClassifyContext,
        Filter,
        ClassifyOut,
        AF_INET,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_LOCAL_ADDRESS,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_LOCAL_PORT,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_REMOTE_ADDRESS,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_REMOTE_PORT,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_PROTOCOL);
}

VOID NTAPI
AleRedirectConnectV6(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_opt_ const VOID* ClassifyContext,
    _In_ const FWPS_FILTER1* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(FlowContext);

    RedirectConnectCommon(
        InFixedValues,
        InMetaValues,
        LayerData,
        ClassifyContext,
        Filter,
        ClassifyOut,
        AF_INET6,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_LOCAL_ADDRESS,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_LOCAL_PORT,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_REMOTE_ADDRESS,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_REMOTE_PORT,
        FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_PROTOCOL);
}

static
VOID
BindRedirectFlowCommon(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _In_ UINT8 AddressFamily,
    _In_ UINT32 LocalAddrIndex,
    _In_ UINT32 LocalPortIndex,
    _In_ UINT32 ProtocolIndex,
    _In_ UINT16 LayerId,
    _In_ UINT32 CalloutId
    )
{
    ULONG processId = 0;
    UINT8 protocol = 0;
    UINT16 localPort = 0;
    UCHAR localAddress[PS_REDIRECT_ADDR_MAX] = { 0 };
    NTSTATUS status;

    if (InFixedValues == NULL || InMetaValues == NULL ||
        !FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_ID) ||
        !FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_FLOW_HANDLE)) {
        return;
    }

    processId = (ULONG)InMetaValues->processId;
    protocol = InFixedValues->incomingValue[ProtocolIndex].value.uint8;
    localPort = InFixedValues->incomingValue[LocalPortIndex].value.uint16;

    if (AddressFamily == AF_INET) {
        UINT32 localV4 = InFixedValues->incomingValue[LocalAddrIndex].value.uint32;
        RtlCopyMemory(localAddress, &localV4, sizeof(localV4));
    } else {
        const FWP_BYTE_ARRAY16* localV6 = InFixedValues->incomingValue[LocalAddrIndex].value.byteArray16;
        if (localV6 == NULL) {
            return;
        }
        RtlCopyMemory(localAddress, localV6->byteArray16, 16);
    }

    status = WfpRedirectBindFlow(
        processId,
        protocol,
        AddressFamily,
        localPort,
        localAddress,
        InMetaValues->flowHandle,
        LayerId);
    if (NT_SUCCESS(status)) {
        FwpsFlowAssociateContext0(
            InMetaValues->flowHandle,
            LayerId,
            CalloutId,
            InMetaValues->flowHandle);
    }
}

VOID NTAPI
AleFlowEstablishedV4(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_ const FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(LayerData);
    UNREFERENCED_PARAMETER(FlowContext);

    if (ClassifyOut != NULL && (ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) != 0) {
        ClassifyOut->actionType = FWP_ACTION_CONTINUE;
    }
    if (Filter == NULL) {
        return;
    }

    BindRedirectFlowCommon(
        InFixedValues,
        InMetaValues,
        AF_INET,
        FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_LOCAL_ADDRESS,
        FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_LOCAL_PORT,
        FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_PROTOCOL,
        FWPS_LAYER_ALE_FLOW_ESTABLISHED_V4,
        Filter->action.calloutId);
}

VOID NTAPI
AleFlowEstablishedV6(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_ const FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(LayerData);
    UNREFERENCED_PARAMETER(FlowContext);

    if (ClassifyOut != NULL && (ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) != 0) {
        ClassifyOut->actionType = FWP_ACTION_CONTINUE;
    }
    if (Filter == NULL) {
        return;
    }

    BindRedirectFlowCommon(
        InFixedValues,
        InMetaValues,
        AF_INET6,
        FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_IP_LOCAL_ADDRESS,
        FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_IP_LOCAL_PORT,
        FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_IP_PROTOCOL,
        FWPS_LAYER_ALE_FLOW_ESTABLISHED_V6,
        Filter->action.calloutId);
}

VOID NTAPI
AleFlowDeleteNotify(
    _In_ UINT16 LayerId,
    _In_ UINT32 CalloutId,
    _In_ UINT64 FlowContext
    )
{
    UNREFERENCED_PARAMETER(CalloutId);
    WfpRedirectFlowDelete(FlowContext, LayerId);
}
