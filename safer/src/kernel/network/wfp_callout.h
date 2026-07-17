/*
 * wfp_callout.h - WFP callout registration and status helpers
 */

#pragma once

#include <fltKernel.h>
#include <fwpsk.h>
#include <fwpmk.h>
#include "../common/shared_types.h"

#pragma comment(lib, "fwpkclnt.lib")

typedef enum _WFP_INJECTION_POINT {
    WFP_ALE_AUTH_CONNECT_V4 = 0,
    WFP_ALE_AUTH_CONNECT_V6,
    WFP_ALE_AUTH_RECV_ACCEPT_V4,
    WFP_ALE_AUTH_RECV_ACCEPT_V6,
    WFP_ALE_FLOW_ESTABLISHED_V4,
    WFP_ALE_FLOW_ESTABLISHED_V6,
    WFP_ALE_DATA_SEND_V4,
    WFP_ALE_DATA_SEND_V6,
    WFP_ALE_DATA_RECV_V4,
    WFP_ALE_DATA_RECV_V6,
    WFP_CLASSIFY_LAYER
} WFP_INJECTION_POINT;

NTSTATUS
WfpCalloutInitialize(VOID);

VOID
WfpCalloutCleanup(VOID);

BOOLEAN
WfpCalloutIsInitialized(VOID);

NTSTATUS
WfpCalloutGetLastError(VOID);

NTSTATUS
WfpRedirectSetSettings(
    _In_ const PS_REDIRECT_SETTINGS* Settings
);

NTSTATUS
WfpRedirectGetSettings(
    _Out_ PS_REDIRECT_SETTINGS* Settings
);

NTSTATUS
WfpRedirectQueryOriginalDestination(
    _Inout_ PS_REDIRECT_QUERY* Query
);

BOOLEAN
WfpRedirectIsActive(VOID);

HANDLE
WfpRedirectGetHandle(VOID);

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
);

NTSTATUS
WfpRedirectBindFlow(
    _In_ ULONG ProcessId,
    _In_ UINT8 Protocol,
    _In_ UINT8 AddressFamily,
    _In_ UINT16 LocalPort,
    _In_reads_(PS_REDIRECT_ADDR_MAX) const UCHAR* LocalAddress,
    _In_ UINT64 FlowId,
    _In_ UINT16 LayerId
);

VOID
WfpRedirectFlowDelete(
    _In_ UINT64 FlowId,
    _In_ UINT16 LayerId
);

NTSTATUS
WfpCalloutRegister(
    _In_ WFP_INJECTION_POINT InjectionPoint,
    _In_ GUID *CalloutGuid
);

NTSTATUS
WfpCalloutUnregister(
    _In_ GUID *CalloutGuid
);

NTSTATUS
WfpCalloutAddFilter(
    _In_ GUID *CalloutGuid,
    _In_ WFP_INJECTION_POINT Layer,
    _In_ UINT16 RemotePort,
    _In_ BOOLEAN Allow,
    _Out_ UINT64 *FilterId
);

NTSTATUS
WfpCalloutRemoveFilter(
    _In_ UINT64 FilterId
);
