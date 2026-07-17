/*
 * classification.h - stream payload inspection helpers and callouts
 */

#pragma once

#include <fltKernel.h>
#include <fwpsk.h>
#include "../common/shared_types.h"

NTSTATUS
ClassificationInitialize(VOID);

VOID
ClassificationCleanup(VOID);

VOID NTAPI
StreamInspectOutboundV4(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _Inout_opt_ VOID* layerData,
    _In_ const FWPS_FILTER0* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* classifyOut
);

VOID NTAPI
StreamInspectOutboundV6(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _Inout_opt_ VOID* layerData,
    _In_ const FWPS_FILTER0* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* classifyOut
);

BOOLEAN
ClassificationTryMatchFtpDataChannelV4(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _In_ DLP_EVENT_TYPE eventType,
    _Out_ PDLP_NET_EVENT event
);

BOOLEAN
ClassificationTryMatchFtpDataChannelV6(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_opt_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _In_ DLP_EVENT_TYPE eventType,
    _Out_ PDLP_NET_EVENT event
);

NTSTATUS
ParseHttpTraffic(
    _In_reads_(packetLength) const UCHAR *packetData,
    _In_ ULONG packetLength,
    _Out_ PWSTR *parsedUrl,
    _Out_ PULONG parsedUrlLength
);
