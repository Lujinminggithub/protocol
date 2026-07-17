#include "protect.h"

#include <bcrypt.h>

#define PS_PROTECT_CHALLENGE_SECONDS 30ULL
#define PS_PROTECT_PROCESS_ALTITUDE L"379951"
#define PS_PROTECT_REGISTRY_ALTITUDE L"379952"
#define PS_PROTECT_MAX_DYNAMIC_PIDS 64
#define PS_PROTECT_MAX_SYSTEM_PIDS 256
#define PS_PROCESS_TERMINATE         0x0001UL
#define PS_PROCESS_CREATE_THREAD     0x0002UL
#define PS_PROCESS_VM_OPERATION      0x0008UL
#define PS_PROCESS_VM_WRITE          0x0020UL
#define PS_PROCESS_DUP_HANDLE        0x0040UL
#define PS_PROCESS_SET_INFORMATION   0x0200UL
#define PS_PROCESS_SUSPEND_RESUME    0x0800UL
#define PS_PROTECT_DENIED_PROCESS_ACCESS \
    (PS_PROCESS_TERMINATE | PS_PROCESS_VM_WRITE | PS_PROCESS_VM_OPERATION | \
     PS_PROCESS_CREATE_THREAD | PS_PROCESS_SUSPEND_RESUME | \
     PS_PROCESS_SET_INFORMATION | PS_PROCESS_DUP_HANDLE)

typedef struct _PS_PROTECT_CONTEXT {
    FAST_MUTEX ControlMutex;
    KSPIN_LOCK ListLock;
    KTIMER RelockTimer;
    KDPC RelockDpc;
    volatile LONG Enabled;
    volatile LONG Mode;
    volatile LONG64 UnlockExpire;
    volatile LONG FailedUnlocks;
    BOOLEAN ChallengeActive;
    PS_PROTECT_TICKET Challenge;
    UCHAR BootId[PS_PROTECT_BOOT_ID_LEN];
    ULONG PathCount;
    WCHAR Paths[PS_PROTECT_MAX_PATHS][PS_PROTECT_PATH_LEN];
    ULONG PidCount;
    ULONG Pids[PS_PROTECT_MAX_PIDS];
    ULONG ProcessImageCount;
    WCHAR ProcessImages[PS_PROTECT_MAX_PROCESS_IMAGES][PS_PROTECT_PATH_LEN];
    ULONG DynamicPidCount;
    ULONG DynamicPids[PS_PROTECT_MAX_DYNAMIC_PIDS];
    ULONG SystemPidCount;
    ULONG SystemPids[PS_PROTECT_MAX_SYSTEM_PIDS];
    PVOID ObRegistrationHandle;
    LARGE_INTEGER RegistryCookie;
    volatile LONG RegistryRegistered;
    volatile LONG Initialized;
    volatile LONG BootstrapComplete;
} PS_PROTECT_CONTEXT;

static PS_PROTECT_CONTEXT gProtect;
static const UCHAR gProtectControlToken[PS_PROTECT_CONTROL_TOKEN_LEN] = {
    0x50,0x53,0x2D,0x42,0x4F,0x4F,0x54,0x2D,0x32,0x30,0x32,0x36,0x2D,0x56,0x31,0x2D,
    0x71,0x37,0x4B,0x39,0x6D,0x32,0x52,0x34,0x78,0x38,0x54,0x33,0x63,0x35,0x4E,0x21
};

static __forceinline WCHAR
ProtectFoldPathChar(_In_ WCHAR Character)
{
    return Character >= L'a' && Character <= L'z'
        ? (WCHAR)(Character - (L'a' - L'A'))
        : Character;
}

static BOOLEAN
ProtectFixedPathEquals(_In_z_ const WCHAR* Left, _In_z_ const WCHAR* Right)
{
    SIZE_T i;
    for (i = 0; i < PS_PROTECT_PATH_LEN; ++i) {
        if (ProtectFoldPathChar(Left[i]) != ProtectFoldPathChar(Right[i])) return FALSE;
        if (Left[i] == L'\0') return TRUE;
    }
    return FALSE;
}

/* BCRYPT_RSAPUBLIC_BLOB for the product maintenance verification key. */
static const UCHAR gProtectPublicKey[] = {
    0x52,0x53,0x41,0x31,0x00,0x08,0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x01,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0xC4,0x47,0xE4,0xEC,0x9F,
    0x94,0x5C,0x36,0xE9,0xE2,0x2E,0xAD,0x1D,0x3A,0x2C,0xDE,0x9B,0x69,0xF3,0x70,0x63,
    0xAC,0x9F,0xFE,0xA4,0x4B,0xB7,0xDC,0xD2,0x60,0x5A,0x60,0xA2,0x51,0xAC,0x09,0x73,
    0xC6,0xBD,0xC0,0x3D,0xE5,0x68,0xC8,0xE4,0x1A,0x71,0x21,0x5B,0x25,0xA5,0x04,0x03,
    0x69,0x16,0x70,0x36,0x79,0x4D,0xB5,0x65,0xFA,0x63,0x6F,0x1A,0xCD,0xD9,0x8E,0x72,
    0xE8,0xEE,0x50,0xE7,0xC5,0x38,0xEF,0xB3,0xB4,0xDF,0x64,0xEB,0xFA,0xE6,0x3B,0x98,
    0x67,0x0C,0xB0,0xF5,0xDF,0xDD,0xEC,0xD3,0xBF,0x36,0xB0,0xF8,0x36,0xCE,0xB9,0x99,
    0xEB,0x2D,0xB6,0x92,0x4F,0x32,0xA9,0x9B,0x5D,0x7A,0x64,0xDC,0x98,0xA1,0x26,0xE4,
    0xEF,0x86,0xDC,0xED,0x28,0x2A,0xC4,0x53,0x69,0x76,0x59,0x6E,0x0E,0xAD,0x85,0x6D,
    0xEF,0xAF,0x45,0xFB,0x0D,0x2D,0x30,0x4B,0x94,0xB0,0xCE,0xDE,0x49,0xE0,0x5B,0xDC,
    0x02,0x17,0x45,0x02,0xE8,0xDC,0xFA,0x4A,0xDC,0x91,0xD7,0xCA,0x4D,0x4A,0x35,0x42,
    0x98,0x35,0xEA,0x9E,0x6D,0x54,0xD8,0x2B,0xAC,0x30,0xDC,0xBF,0xD1,0xC0,0x40,0x41,
    0xD9,0x8F,0x45,0x8B,0x45,0xD7,0x94,0x4E,0x0E,0xB8,0x2E,0xF1,0xFC,0x15,0xA4,0x08,
    0x49,0xF3,0x27,0x21,0xE3,0xCF,0xB6,0x27,0x7C,0x46,0x35,0x6F,0x96,0x9D,0x1B,0xE0,
    0x5A,0xB8,0xF3,0xB5,0xEF,0xF0,0xB0,0x8D,0x3F,0x09,0x6F,0x88,0xEA,0xAC,0x85,0x77,
    0x02,0xD4,0x52,0xA6,0x88,0xBB,0x10,0xA2,0xBD,0xC0,0x96,0x53,0x75,0x22,0xB9,0xD3,
    0x5D,0x4F,0x71,0xD0,0x99,0xF4,0x04,0x44,0x69,0xDD,0x83
};

static BOOLEAN
ProtectPathPrefixMatches(_In_z_ const WCHAR* Path, _In_z_ const WCHAR* Prefix)
{
    SIZE_T i;

    for (i = 0; Prefix[i] != L'\0'; ++i) {
        if (Path[i] == L'\0' || ProtectFoldPathChar(Path[i]) != ProtectFoldPathChar(Prefix[i])) {
            return FALSE;
        }
    }

    return i > 0 &&
        (Prefix[i - 1] == L'\\' || Path[i] == L'\0' || Path[i] == L'\\');
}

static BOOLEAN
ProtectUnicodePathMatches(_In_ PCUNICODE_STRING Path, _In_z_ const WCHAR* Prefix)
{
    USHORT prefixChars = 0;
    USHORT pathChars;
    USHORT i;

    if (Path == NULL || Path->Buffer == NULL) return FALSE;
    while (Prefix[prefixChars] != L'\0') ++prefixChars;
    pathChars = Path->Length / sizeof(WCHAR);
    if (pathChars < prefixChars) return FALSE;
    for (i = 0; i < prefixChars; ++i) {
        if (ProtectFoldPathChar(Path->Buffer[i]) != ProtectFoldPathChar(Prefix[i])) return FALSE;
    }
    return pathChars == prefixChars || Prefix[prefixChars - 1] == L'\\' ||
        Path->Buffer[prefixChars] == L'\\';
}

static BOOLEAN
ProtectUnicodeContainsAt(
    _In_ PCUNICODE_STRING Path,
    _In_ USHORT OffsetChars,
    _In_z_ const WCHAR* Value,
    _Out_ PUSHORT EndChars
    )
{
    USHORT pathChars = Path->Length / sizeof(WCHAR);
    USHORT i = 0;

    while (Value[i] != L'\0') {
        if ((USHORT)(OffsetChars + i) >= pathChars ||
            ProtectFoldPathChar(Path->Buffer[OffsetChars + i]) != ProtectFoldPathChar(Value[i])) {
            return FALSE;
        }
        ++i;
    }
    *EndChars = (USHORT)(OffsetChars + i);
    return TRUE;
}

static BOOLEAN
ProtectImageIsOperatingSystem(_In_ PCUNICODE_STRING ImagePath)
{
    static const WCHAR systemRoot[] = L"\\SystemRoot\\";
    static const WCHAR windowsSegment[] = L"\\Windows\\";
    USHORT pathChars;
    USHORT offset;

    if (ImagePath == NULL || ImagePath->Buffer == NULL) return FALSE;
    if (ProtectUnicodePathMatches(ImagePath, systemRoot)) return TRUE;
    pathChars = ImagePath->Length / sizeof(WCHAR);
    for (offset = 0; offset < pathChars; ++offset) {
        USHORT endChars;
        if (ProtectUnicodeContainsAt(ImagePath, offset, windowsSegment, &endChars)) return TRUE;
    }
    return FALSE;
}

static BOOLEAN
ProtectRegistryObjectMatches(_In_ PVOID Object)
{
    static const WCHAR systemRoot[] = L"\\REGISTRY\\MACHINE\\SYSTEM\\";
    static const WCHAR serviceSuffix[] = L"\\Services\\PersonalSafer";
    PCUNICODE_STRING objectName = NULL;
    BOOLEAN matched = FALSE;
    USHORT rootChars = (USHORT)(RTL_NUMBER_OF(systemRoot) - 1);
    USHORT pathChars;
    USHORT offsetChars;

    if (Object == NULL ||
        !NT_SUCCESS(CmCallbackGetKeyObjectIDEx(
            &gProtect.RegistryCookie, Object, NULL, &objectName, 0))) {
        return FALSE;
    }
    pathChars = objectName->Length / sizeof(WCHAR);
    if (ProtectUnicodePathMatches(objectName, systemRoot)) {
        for (offsetChars = rootChars; offsetChars < pathChars; ++offsetChars) {
            USHORT endChars;
            if (ProtectUnicodeContainsAt(objectName, offsetChars, serviceSuffix, &endChars)) {
                matched = endChars == pathChars || objectName->Buffer[endChars] == L'\\';
                if (matched) break;
            }
        }
    }
    CmCallbackReleaseKeyObjectIDEx(objectName);
    return matched;
}

static NTSTATUS
ProtectRegistryCallback(_In_ PVOID Context, _In_opt_ PVOID Argument1, _In_opt_ PVOID Argument2)
{
    REG_NOTIFY_CLASS notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)Argument1;
    PVOID keyObject = NULL;

    UNREFERENCED_PARAMETER(Context);
    if (!ProtectShouldEnforce() || Argument2 == NULL) return STATUS_SUCCESS;

    switch (notifyClass) {
        case RegNtPreDeleteKey:
            keyObject = ((PREG_DELETE_KEY_INFORMATION)Argument2)->Object;
            break;
        case RegNtPreSetValueKey:
            keyObject = ((PREG_SET_VALUE_KEY_INFORMATION)Argument2)->Object;
            break;
        case RegNtPreDeleteValueKey:
            keyObject = ((PREG_DELETE_VALUE_KEY_INFORMATION)Argument2)->Object;
            break;
        case RegNtPreRenameKey:
            keyObject = ((PREG_RENAME_KEY_INFORMATION)Argument2)->Object;
            break;
        case RegNtPreSetInformationKey:
            keyObject = ((PREG_SET_INFORMATION_KEY_INFORMATION)Argument2)->Object;
            break;
        case RegNtPreSetKeySecurity:
            keyObject = ((PREG_SET_KEY_SECURITY_INFORMATION)Argument2)->Object;
            break;
        default:
            return STATUS_SUCCESS;
    }

    return ProtectRegistryObjectMatches(keyObject) ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

static VOID
ProtectRelockDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    LARGE_INTEGER now;
    LONG64 expiry;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    KeQuerySystemTime(&now);
    expiry = InterlockedCompareExchange64(&gProtect.UnlockExpire, 0, 0);
    if (expiry != 0 && expiry <= now.QuadPart) {
        InterlockedExchange(&gProtect.Mode, PS_PROTECT_MODE_LOCKED);
        InterlockedExchange64(&gProtect.UnlockExpire, 0);
    }
}

static BOOLEAN
ProtectVerifySignature(_In_ const PS_PROTECT_UNLOCK_REQUEST* Request)
{
    BCRYPT_ALG_HANDLE rsaAlgorithm = NULL;
    BCRYPT_ALG_HANDLE hashAlgorithm = NULL;
    BCRYPT_KEY_HANDLE publicKey = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BCRYPT_PKCS1_PADDING_INFO paddingInfo;
    PUCHAR hashObject = NULL;
    ULONG hashObjectLength = 0;
    ULONG resultLength = 0;
    UCHAR digest[32];
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&rsaAlgorithm, BCRYPT_RSA_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) goto Cleanup;
    status = BCryptImportKeyPair(rsaAlgorithm, NULL, BCRYPT_RSAPUBLIC_BLOB, &publicKey,
        (PUCHAR)gProtectPublicKey, sizeof(gProtectPublicKey), 0);
    if (!NT_SUCCESS(status)) goto Cleanup;
    status = BCryptOpenAlgorithmProvider(&hashAlgorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) goto Cleanup;
    status = BCryptGetProperty(hashAlgorithm, BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&hashObjectLength, sizeof(hashObjectLength), &resultLength, 0);
    if (!NT_SUCCESS(status) || hashObjectLength == 0) goto Cleanup;
    hashObject = (PUCHAR)ExAllocatePoolZero(NonPagedPoolNx, hashObjectLength, 'hspS');
    if (hashObject == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    status = BCryptCreateHash(hashAlgorithm, &hash, hashObject, hashObjectLength, NULL, 0, 0);
    if (!NT_SUCCESS(status)) goto Cleanup;
    status = BCryptHashData(hash, (PUCHAR)&Request->Ticket, sizeof(Request->Ticket), 0);
    if (!NT_SUCCESS(status)) goto Cleanup;
    status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (!NT_SUCCESS(status)) goto Cleanup;

    paddingInfo.pszAlgId = BCRYPT_SHA256_ALGORITHM;
    status = BCryptVerifySignature(publicKey, &paddingInfo, digest, sizeof(digest),
        (PUCHAR)Request->Signature, Request->SignatureLength, BCRYPT_PAD_PKCS1);

Cleanup:
    if (hash != NULL) BCryptDestroyHash(hash);
    if (hashObject != NULL) ExFreePoolWithTag(hashObject, 'hspS');
    if (hashAlgorithm != NULL) BCryptCloseAlgorithmProvider(hashAlgorithm, 0);
    if (publicKey != NULL) BCryptDestroyKey(publicKey);
    if (rsaAlgorithm != NULL) BCryptCloseAlgorithmProvider(rsaAlgorithm, 0);
    RtlSecureZeroMemory(digest, sizeof(digest));
    return NT_SUCCESS(status);
}

static OB_PREOP_CALLBACK_STATUS
ProtectProcessPreOperation(_In_ PVOID RegistrationContext, _Inout_ POB_PRE_OPERATION_INFORMATION Info)
{
    ULONG targetPid;
    ACCESS_MASK* desiredAccess;

    UNREFERENCED_PARAMETER(RegistrationContext);
    if (Info->ObjectType != *PsProcessType || Info->KernelHandle || !ProtectShouldEnforce()) {
        return OB_PREOP_SUCCESS;
    }

    targetPid = HandleToULong(PsGetProcessId((PEPROCESS)Info->Object));
    if (targetPid == HandleToULong(PsGetCurrentProcessId()) || !ProtectIsProcessProtected(targetPid)) {
        return OB_PREOP_SUCCESS;
    }

    desiredAccess = Info->Operation == OB_OPERATION_HANDLE_CREATE
        ? &Info->Parameters->CreateHandleInformation.DesiredAccess
        : &Info->Parameters->DuplicateHandleInformation.DesiredAccess;
    *desiredAccess &= ~PS_PROTECT_DENIED_PROCESS_ACCESS;
    return OB_PREOP_SUCCESS;
}

NTSTATUS
ProtectInitialize(_In_ PDRIVER_OBJECT DriverObject)
{
    OB_CALLBACK_REGISTRATION registration;
    OB_OPERATION_REGISTRATION operation;
    UNICODE_STRING altitude;
    NTSTATUS status;

    RtlZeroMemory(&gProtect, sizeof(gProtect));
    ExInitializeFastMutex(&gProtect.ControlMutex);
    KeInitializeSpinLock(&gProtect.ListLock);
    KeInitializeTimer(&gProtect.RelockTimer);
    KeInitializeDpc(&gProtect.RelockDpc, ProtectRelockDpc, NULL);
    gProtect.Enabled = TRUE;
    gProtect.Mode = PS_PROTECT_MODE_LOCKED;

    status = BCryptGenRandom(NULL, gProtect.BootId, sizeof(gProtect.BootId), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&operation, sizeof(operation));
    operation.ObjectType = PsProcessType;
    operation.Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    operation.PreOperation = ProtectProcessPreOperation;

    RtlInitUnicodeString(&altitude, PS_PROTECT_PROCESS_ALTITUDE);
    RtlZeroMemory(&registration, sizeof(registration));
    registration.Version = OB_FLT_REGISTRATION_VERSION;
    registration.OperationRegistrationCount = 1;
    registration.Altitude = altitude;
    registration.OperationRegistration = &operation;
    status = ObRegisterCallbacks(&registration, &gProtect.ObRegistrationHandle);
    if (!NT_SUCCESS(status)) {
        RtlSecureZeroMemory(&gProtect, sizeof(gProtect));
        return status;
    }

    RtlInitUnicodeString(&altitude, PS_PROTECT_REGISTRY_ALTITUDE);
    status = CmRegisterCallbackEx(
        ProtectRegistryCallback,
        &altitude,
        DriverObject,
        NULL,
        &gProtect.RegistryCookie,
        NULL);
    if (!NT_SUCCESS(status)) {
        ObUnRegisterCallbacks(gProtect.ObRegistrationHandle);
        gProtect.ObRegistrationHandle = NULL;
        RtlSecureZeroMemory(&gProtect, sizeof(gProtect));
        return status;
    }
    gProtect.RegistryRegistered = TRUE;
    InterlockedExchange(&gProtect.Initialized, TRUE);
    return STATUS_SUCCESS;
}

VOID
ProtectCleanup(VOID)
{
    InterlockedExchange(&gProtect.Initialized, FALSE);
    if (InterlockedExchange(&gProtect.RegistryRegistered, FALSE)) {
        CmUnRegisterCallback(gProtect.RegistryCookie);
    }
    if (gProtect.ObRegistrationHandle != NULL) {
        ObUnRegisterCallbacks(gProtect.ObRegistrationHandle);
        gProtect.ObRegistrationHandle = NULL;
    }
    KeCancelTimer(&gProtect.RelockTimer);
    KeFlushQueuedDpcs();
    RtlSecureZeroMemory(&gProtect, sizeof(gProtect));
}

BOOLEAN
ProtectShouldEnforce(VOID)
{
    LARGE_INTEGER now;
    LONG64 expiry;

    if (InterlockedCompareExchange(&gProtect.Enabled, 0, 0) == 0) return FALSE;
    if (InterlockedCompareExchange(&gProtect.Mode, 0, 0) == PS_PROTECT_MODE_LOCKED) return TRUE;
    expiry = InterlockedCompareExchange64(&gProtect.UnlockExpire, 0, 0);
    KeQuerySystemTime(&now);
    if (expiry <= now.QuadPart) {
        InterlockedExchange(&gProtect.Mode, PS_PROTECT_MODE_LOCKED);
        InterlockedExchange64(&gProtect.UnlockExpire, 0);
        return TRUE;
    }
    return FALSE;
}

BOOLEAN ProtectCanUnload(VOID) { return !ProtectShouldEnforce(); }

BOOLEAN
ProtectIsPathProtected(_In_z_ const WCHAR* Path)
{
    KIRQL oldIrql;
    ULONG i;
    BOOLEAN matched = FALSE;

    if (Path == NULL || !ProtectShouldEnforce()) return FALSE;
    KeAcquireSpinLock(&gProtect.ListLock, &oldIrql);
    for (i = 0; i < gProtect.PathCount; ++i) {
        if (ProtectPathPrefixMatches(Path, gProtect.Paths[i])) {
            matched = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
    return matched;
}

BOOLEAN
ProtectIsProcessProtected(_In_ ULONG ProcessId)
{
    KIRQL oldIrql;
    ULONG i;
    BOOLEAN matched = FALSE;

    KeAcquireSpinLock(&gProtect.ListLock, &oldIrql);
    for (i = 0; i < gProtect.PidCount; ++i) {
        if (gProtect.Pids[i] == ProcessId) {
            matched = TRUE;
            break;
        }
    }
    if (!matched) {
        for (i = 0; i < gProtect.DynamicPidCount; ++i) {
            if (gProtect.DynamicPids[i] == ProcessId) {
                matched = TRUE;
                break;
            }
        }
    }
    KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
    return matched;
}

BOOLEAN
ProtectIsMaintenanceProcess(_In_ ULONG ProcessId)
{
    if (ProtectShouldEnforce()) return FALSE;
    return ProtectIsProcessProtected(ProcessId);
}

BOOLEAN
ProtectShouldBypassDlp(_In_ ULONG ProcessId)
{
    KIRQL oldIrql;
    ULONG i;
    BOOLEAN bypass = ProcessId == 0 || ProcessId == 4;

    if (bypass) return TRUE;
    KeAcquireSpinLock(&gProtect.ListLock, &oldIrql);
    for (i = 0; i < gProtect.PidCount && !bypass; ++i) bypass = gProtect.Pids[i] == ProcessId;
    for (i = 0; i < gProtect.DynamicPidCount && !bypass; ++i) bypass = gProtect.DynamicPids[i] == ProcessId;
    for (i = 0; i < gProtect.SystemPidCount && !bypass; ++i) bypass = gProtect.SystemPids[i] == ProcessId;
    KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
    return bypass;
}

VOID
ProtectProcessStarted(_In_ ULONG ProcessId, _In_opt_ PCUNICODE_STRING ImagePath)
{
    KIRQL oldIrql;
    ULONG i;
    BOOLEAN matched = FALSE;
    BOOLEAN systemProcess;

    if (InterlockedCompareExchange(&gProtect.Initialized, 0, 0) == FALSE ||
        ProcessId == 0 || ImagePath == NULL || ImagePath->Buffer == NULL) return;

    systemProcess = ProtectImageIsOperatingSystem(ImagePath);
    KeAcquireSpinLock(&gProtect.ListLock, &oldIrql);
    for (i = 0; i < gProtect.ProcessImageCount; ++i) {
        if (ProtectUnicodePathMatches(ImagePath, gProtect.ProcessImages[i])) {
            matched = TRUE;
            break;
        }
    }
    if (matched) {
        for (i = 0; i < gProtect.DynamicPidCount; ++i) {
            if (gProtect.DynamicPids[i] == ProcessId) {
                matched = FALSE;
                break;
            }
        }
        if (matched && gProtect.DynamicPidCount < PS_PROTECT_MAX_DYNAMIC_PIDS) {
            gProtect.DynamicPids[gProtect.DynamicPidCount++] = ProcessId;
        }
    }
    if (systemProcess) {
        for (i = 0; i < gProtect.SystemPidCount; ++i) {
            if (gProtect.SystemPids[i] == ProcessId) break;
        }
        if (i == gProtect.SystemPidCount && gProtect.SystemPidCount < PS_PROTECT_MAX_SYSTEM_PIDS) {
            gProtect.SystemPids[gProtect.SystemPidCount++] = ProcessId;
        }
    }
    KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
}

VOID
ProtectObserveProcess(_In_ PEPROCESS Process)
{
    PUNICODE_STRING imagePath = NULL;

    PAGED_CODE();
    if (Process == NULL || InterlockedCompareExchange(&gProtect.Initialized, 0, 0) == FALSE) return;
    if (NT_SUCCESS(SeLocateProcessImageName(Process, &imagePath)) && imagePath != NULL) {
        ProtectProcessStarted(HandleToULong(PsGetProcessId(Process)), imagePath);
        ExFreePool(imagePath);
    }
}

VOID
ProtectProcessStopped(_In_ ULONG ProcessId)
{
    KIRQL oldIrql;
    ULONG i;

    if (InterlockedCompareExchange(&gProtect.Initialized, 0, 0) == FALSE) return;
    KeAcquireSpinLock(&gProtect.ListLock, &oldIrql);
    for (i = 0; i < gProtect.PidCount; ++i) {
        if (gProtect.Pids[i] == ProcessId) {
            gProtect.Pids[i] = gProtect.Pids[gProtect.PidCount - 1];
            --gProtect.PidCount;
            break;
        }
    }
    for (i = 0; i < gProtect.DynamicPidCount; ++i) {
        if (gProtect.DynamicPids[i] == ProcessId) {
            gProtect.DynamicPids[i] = gProtect.DynamicPids[gProtect.DynamicPidCount - 1];
            --gProtect.DynamicPidCount;
            break;
        }
    }
    for (i = 0; i < gProtect.SystemPidCount; ++i) {
        if (gProtect.SystemPids[i] == ProcessId) {
            gProtect.SystemPids[i] = gProtect.SystemPids[gProtect.SystemPidCount - 1];
            --gProtect.SystemPidCount;
            break;
        }
    }
    KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
}

NTSTATUS
ProtectControl(_In_ const PS_PROTECT_CONTROL* Control)
{
    ULONG i;
    UCHAR difference = 0;
    KIRQL oldIrql;
    LARGE_INTEGER now;
    LARGE_INTEGER dueTime;

    PAGED_CODE();
    if (Control == NULL || Control->Version != PS_PROTECT_VERSION) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < PS_PROTECT_CONTROL_TOKEN_LEN; ++i) {
        difference |= Control->Token[i] ^ gProtectControlToken[i];
    }
    if (difference != 0) return STATUS_ACCESS_DENIED;

    if (Control->Action == PS_PROTECT_CONTROL_RELOCK) {
        ProtectRelock();
        return STATUS_SUCCESS;
    }
    if (Control->Action == PS_PROTECT_CONTROL_UNLOCK) {
        if (InterlockedCompareExchange(&gProtect.BootstrapComplete, 0, 0) == FALSE) {
            return STATUS_DEVICE_NOT_READY;
        }
        KeQuerySystemTime(&now);
        InterlockedExchange64(&gProtect.UnlockExpire,
            now.QuadPart + (LONG64)PS_PROTECT_MAX_TTL_SECONDS * 10000000LL);
        InterlockedExchange(&gProtect.Mode, PS_PROTECT_MODE_UNLOCKED);
        dueTime.QuadPart = -(LONG64)PS_PROTECT_MAX_TTL_SECONDS * 10000000LL;
        KeSetTimer(&gProtect.RelockTimer, dueTime, &gProtect.RelockDpc);
        return STATUS_SUCCESS;
    }
    if (Control->Action != PS_PROTECT_CONTROL_INITIALIZE || Control->ProcessId == 0 ||
        Control->InstallDirectory[0] != L'\\' || Control->ProcessImage[0] != L'\\' ||
        Control->DriverPath[0] != L'\\' ||
        Control->InstallDirectory[PS_PROTECT_PATH_LEN - 1] != L'\0' ||
        Control->ProcessImage[PS_PROTECT_PATH_LEN - 1] != L'\0' ||
        Control->DriverPath[PS_PROTECT_PATH_LEN - 1] != L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&gProtect.ListLock, &oldIrql);
    if (gProtect.BootstrapComplete) {
        if (!ProtectFixedPathEquals(gProtect.Paths[0], Control->InstallDirectory) ||
            !ProtectFixedPathEquals(gProtect.Paths[1], Control->DriverPath) ||
            !ProtectFixedPathEquals(gProtect.ProcessImages[0], Control->ProcessImage)) {
            KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
            return STATUS_ACCESS_DENIED;
        }
        for (i = 0; i < gProtect.DynamicPidCount; ++i) {
            if (gProtect.DynamicPids[i] == Control->ProcessId) break;
        }
        if (i == gProtect.DynamicPidCount &&
            gProtect.DynamicPidCount < PS_PROTECT_MAX_DYNAMIC_PIDS) {
            gProtect.DynamicPids[gProtect.DynamicPidCount++] = Control->ProcessId;
        }
        KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
        ProtectRelock();
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(gProtect.Paths, sizeof(gProtect.Paths));
    RtlZeroMemory(gProtect.Pids, sizeof(gProtect.Pids));
    RtlZeroMemory(gProtect.ProcessImages, sizeof(gProtect.ProcessImages));
    RtlZeroMemory(gProtect.DynamicPids, sizeof(gProtect.DynamicPids));
    RtlCopyMemory(gProtect.Paths[0], Control->InstallDirectory, sizeof(gProtect.Paths[0]));
    RtlCopyMemory(gProtect.Paths[1], Control->DriverPath, sizeof(gProtect.Paths[1]));
    RtlCopyMemory(gProtect.ProcessImages[0], Control->ProcessImage, sizeof(gProtect.ProcessImages[0]));
    gProtect.PathCount = 2;
    gProtect.PidCount = 1;
    gProtect.Pids[0] = Control->ProcessId;
    gProtect.ProcessImageCount = 1;
    gProtect.DynamicPidCount = 0;
    InterlockedExchange(&gProtect.Enabled, TRUE);
    InterlockedExchange(&gProtect.BootstrapComplete, TRUE);
    KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
    ProtectRelock();
    return STATUS_SUCCESS;
}

NTSTATUS
ProtectSetList(_In_ const PS_PROTECT_LIST* List)
{
    KIRQL oldIrql;
    ULONG i;

    PAGED_CODE();
    if (List == NULL || List->PathCount > PS_PROTECT_MAX_PATHS ||
        List->PidCount > PS_PROTECT_MAX_PIDS ||
        List->ProcessImageCount > PS_PROTECT_MAX_PROCESS_IMAGES) {
        return STATUS_INVALID_PARAMETER;
    }
    if (ProtectShouldEnforce()) return STATUS_ACCESS_DENIED;
    for (i = 0; i < List->PathCount; ++i) {
        if (List->Paths[i][0] != L'\\' ||
            List->Paths[i][PS_PROTECT_PATH_LEN - 1] != L'\0') {
            return STATUS_INVALID_PARAMETER;
        }
    }
    for (i = 0; i < List->ProcessImageCount; ++i) {
        if (List->ProcessImages[i][0] != L'\\' ||
            List->ProcessImages[i][PS_PROTECT_PATH_LEN - 1] != L'\0') {
            return STATUS_INVALID_PARAMETER;
        }
    }

    KeAcquireSpinLock(&gProtect.ListLock, &oldIrql);
    RtlZeroMemory(gProtect.Paths, sizeof(gProtect.Paths));
    RtlZeroMemory(gProtect.Pids, sizeof(gProtect.Pids));
    RtlZeroMemory(gProtect.ProcessImages, sizeof(gProtect.ProcessImages));
    RtlZeroMemory(gProtect.DynamicPids, sizeof(gProtect.DynamicPids));
    gProtect.PathCount = List->PathCount;
    gProtect.PidCount = List->PidCount;
    gProtect.ProcessImageCount = List->ProcessImageCount;
    gProtect.DynamicPidCount = 0;
    RtlCopyMemory(gProtect.Paths, List->Paths, sizeof(gProtect.Paths));
    RtlCopyMemory(gProtect.Pids, List->Pids, sizeof(gProtect.Pids));
    RtlCopyMemory(gProtect.ProcessImages, List->ProcessImages, sizeof(gProtect.ProcessImages));
    InterlockedExchange(&gProtect.Enabled, List->Enabled != 0);
    KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS
ProtectGetChallenge(_Out_ PS_PROTECT_TICKET* Ticket)
{
    LARGE_INTEGER now;
    NTSTATUS status;

    PAGED_CODE();
    if (Ticket == NULL) return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&gProtect.ControlMutex);
    RtlZeroMemory(&gProtect.Challenge, sizeof(gProtect.Challenge));
    gProtect.Challenge.Version = PS_PROTECT_VERSION;
    gProtect.Challenge.RequestorPid = HandleToULong(PsGetCurrentProcessId());
    KeQuerySystemTime(&now);
    gProtect.Challenge.ChallengeIssued = (ULONGLONG)now.QuadPart;
    RtlCopyMemory(gProtect.Challenge.BootId, gProtect.BootId, sizeof(gProtect.BootId));
    status = BCryptGenRandom(NULL, gProtect.Challenge.Nonce, sizeof(gProtect.Challenge.Nonce),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (NT_SUCCESS(status)) {
        gProtect.ChallengeActive = TRUE;
        RtlCopyMemory(Ticket, &gProtect.Challenge, sizeof(*Ticket));
    } else {
        gProtect.ChallengeActive = FALSE;
    }
    ExReleaseFastMutex(&gProtect.ControlMutex);
    return status;
}

NTSTATUS
ProtectUnlock(_In_ const PS_PROTECT_UNLOCK_REQUEST* Request)
{
    LARGE_INTEGER now;
    LARGE_INTEGER dueTime;
    ULONGLONG age;
    NTSTATUS status = STATUS_ACCESS_DENIED;

    PAGED_CODE();
    if (Request == NULL || Request->Ticket.Version != PS_PROTECT_VERSION ||
        Request->Ticket.TtlSeconds == 0 || Request->Ticket.TtlSeconds > PS_PROTECT_MAX_TTL_SECONDS ||
        Request->SignatureLength != PS_PROTECT_SIGNATURE_LEN) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&gProtect.ControlMutex);
    KeQuerySystemTime(&now);
    age = now.QuadPart >= (LONGLONG)Request->Ticket.ChallengeIssued
        ? (ULONGLONG)(now.QuadPart - (LONGLONG)Request->Ticket.ChallengeIssued)
        : ~(ULONGLONG)0;
    if (!gProtect.ChallengeActive ||
        Request->Ticket.RequestorPid != HandleToULong(PsGetCurrentProcessId()) ||
        Request->Ticket.RequestorPid != gProtect.Challenge.RequestorPid ||
        Request->Ticket.ChallengeIssued != gProtect.Challenge.ChallengeIssued ||
        age > PS_PROTECT_CHALLENGE_SECONDS * 10000000ULL ||
        RtlCompareMemory(Request->Ticket.BootId, gProtect.Challenge.BootId, PS_PROTECT_BOOT_ID_LEN) != PS_PROTECT_BOOT_ID_LEN ||
        RtlCompareMemory(Request->Ticket.Nonce, gProtect.Challenge.Nonce, PS_PROTECT_NONCE_LEN) != PS_PROTECT_NONCE_LEN) {
        goto Cleanup;
    }

    gProtect.ChallengeActive = FALSE;
    if (!ProtectVerifySignature(Request)) goto Cleanup;

    InterlockedExchange64(&gProtect.UnlockExpire,
        now.QuadPart + (LONG64)Request->Ticket.TtlSeconds * 10000000LL);
    InterlockedExchange(&gProtect.Mode, PS_PROTECT_MODE_UNLOCKED);
    dueTime.QuadPart = -(LONG64)Request->Ticket.TtlSeconds * 10000000LL;
    KeSetTimer(&gProtect.RelockTimer, dueTime, &gProtect.RelockDpc);
    status = STATUS_SUCCESS;

Cleanup:
    if (!NT_SUCCESS(status)) InterlockedIncrement(&gProtect.FailedUnlocks);
    ExReleaseFastMutex(&gProtect.ControlMutex);
    return status;
}

VOID
ProtectRelock(VOID)
{
    KeCancelTimer(&gProtect.RelockTimer);
    InterlockedExchange(&gProtect.Mode, PS_PROTECT_MODE_LOCKED);
    InterlockedExchange64(&gProtect.UnlockExpire, 0);
}

VOID
ProtectQueryState(_Out_ PS_PROTECT_STATUS* Status)
{
    LARGE_INTEGER now;
    LONG64 expiry;
    KIRQL oldIrql;

    RtlZeroMemory(Status, sizeof(*Status));
    Status->Version = PS_PROTECT_VERSION;
    Status->Enabled = InterlockedCompareExchange(&gProtect.Enabled, 0, 0) != 0;
    Status->Mode = ProtectShouldEnforce() ? PS_PROTECT_MODE_LOCKED : PS_PROTECT_MODE_UNLOCKED;
    expiry = InterlockedCompareExchange64(&gProtect.UnlockExpire, 0, 0);
    KeQuerySystemTime(&now);
    if (Status->Mode == PS_PROTECT_MODE_UNLOCKED && expiry > now.QuadPart) {
        Status->RemainingTtlSeconds = (ULONG)((expiry - now.QuadPart + 9999999LL) / 10000000LL);
    }
    ExAcquireFastMutex(&gProtect.ControlMutex);
    Status->ChallengeActive = gProtect.ChallengeActive;
    ExReleaseFastMutex(&gProtect.ControlMutex);
    KeAcquireSpinLock(&gProtect.ListLock, &oldIrql);
    Status->PathCount = gProtect.PathCount;
    Status->PidCount = gProtect.PidCount;
    Status->ProcessImageCount = gProtect.ProcessImageCount;
    KeReleaseSpinLock(&gProtect.ListLock, oldIrql);
    Status->FailedUnlocks = (ULONG)InterlockedCompareExchange(&gProtect.FailedUnlocks, 0, 0);
}
