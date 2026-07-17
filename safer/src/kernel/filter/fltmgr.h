/*
 * fltmgr.h - MiniFilter callbacks and quarantine view state
 */

#pragma once

#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>

#define FILE_CREATE_OPERATION  0x0001
#define FILE_WRITE_OPERATION   0x0002
#define FILE_READ_OPERATION    0x0004
#define FILE_DELETE_OPERATION  0x0008
#define FILE_RENAME_OPERATION  0x0010

typedef struct _PS_QUARANTINE_WRITE_CONTEXT {
    HANDLE QuarantineHandle;
    PFILE_OBJECT QuarantineFileObject;
    BOOLEAN RedirectedByCreate;
    BOOLEAN MappingRegistered;
    BOOLEAN AppendWritesToEnd;
    USHORT OriginalPathLength;
    WCHAR OriginalPath[512];
    USHORT QuarantinePathLength;
    WCHAR QuarantinePath[512];
} PS_QUARANTINE_WRITE_CONTEXT, *PPS_QUARANTINE_WRITE_CONTEXT;

typedef struct _PS_CREATE_REDIRECT_COMPLETION {
    BOOLEAN AppendWritesToEnd;
    USHORT OriginalPathLength;
    WCHAR OriginalPath[512];
    USHORT QuarantinePathLength;
    WCHAR QuarantinePath[512];
} PS_CREATE_REDIRECT_COMPLETION, *PPS_CREATE_REDIRECT_COMPLETION;

VOID
QuarantineWriteContextCleanup(
    _In_ PFLT_CONTEXT Context,
    _In_ FLT_CONTEXT_TYPE ContextType
);

VOID
FltMgrCleanup(VOID);

FLT_PREOP_CALLBACK_STATUS
PreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
);

FLT_POSTOP_CALLBACK_STATUS
PostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
);

FLT_PREOP_CALLBACK_STATUS
PreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
);

FLT_PREOP_CALLBACK_STATUS
PreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
);

FLT_PREOP_CALLBACK_STATUS
PreQueryInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
);

FLT_PREOP_CALLBACK_STATUS
PreDirectoryControl(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
);

FLT_POSTOP_CALLBACK_STATUS
PostDirectoryControl(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
);

FLT_PREOP_CALLBACK_STATUS
PreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
);
