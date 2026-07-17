/*
 * ps_shared.h - user-mode ABI mirror of src/kernel/common/shared_types.h
 */

#pragma once

#include <windows.h>

#define PS_FILE_DEVICE 0x8000
#define PS_CTL_CODE(code) CTL_CODE(PS_FILE_DEVICE, code, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PS_FILE_EVENT     PS_CTL_CODE(0x800)
#define IOCTL_PS_NET_EVENT      PS_CTL_CODE(0x801)
#define IOCTL_PS_DRIVER_START   PS_CTL_CODE(0x802)
#define IOCTL_PS_DRIVER_STOP    PS_CTL_CODE(0x803)
#define IOCTL_PS_SET_POLICY     PS_CTL_CODE(0x804)
#define IOCTL_PS_GET_POLICY     PS_CTL_CODE(0x805)
#define IOCTL_PS_DRIVER_STATUS  PS_CTL_CODE(0x806)
#define IOCTL_PS_HEARTBEAT      PS_CTL_CODE(0x807)
#define IOCTL_PS_SET_QUARANTINE PS_CTL_CODE(0x808)
#define IOCTL_PS_GET_QUARANTINE PS_CTL_CODE(0x809)
#define IOCTL_PS_SET_REDIRECT   PS_CTL_CODE(0x80A)
#define IOCTL_PS_QUERY_REDIRECT PS_CTL_CODE(0x80B)
#define IOCTL_PS_FILE_EVENT_BATCH PS_CTL_CODE(0x80C)
#define IOCTL_PS_NET_EVENT_BATCH  PS_CTL_CODE(0x80D)
#define IOCTL_PS_WAIT_EVENTS      PS_CTL_CODE(0x80E)
#define IOCTL_PS_SET_PROTECT_LIST PS_CTL_CODE(0x80F)
#define IOCTL_PS_GET_PROTECT_CHALLENGE PS_CTL_CODE(0x810)
#define IOCTL_PS_UNLOCK_PROTECTION PS_CTL_CODE(0x811)
#define IOCTL_PS_RELOCK_PROTECTION PS_CTL_CODE(0x812)
#define IOCTL_PS_QUERY_PROTECT_STATE PS_CTL_CODE(0x813)
#define IOCTL_PS_PROTECT_CONTROL PS_CTL_CODE(0x814)

typedef enum _DLP_EVENT_TYPE {
    EventFileCreate = 1,
    EventFileWrite,
    EventFileRead,
    EventFileDelete,
    EventFileRename,
    EventNetworkConnect,
    EventNetworkDisconnect,
    EventHttpRequest,
    EventHttpResponse,
    EventFtpCommand,
    EventRegistryChange,
    EventSniCapture
} DLP_EVENT_TYPE;

typedef enum _ACTION_RESULT {
    ActionAllowed = 0,
    ActionBlocked,
    ActionLogged,
    ActionQuarantined
} ACTION_RESULT;

typedef struct _DLP_FILE_EVENT {
    DLP_EVENT_TYPE EventType;
    LARGE_INTEGER  Timestamp;
    ULONG          ProcessId;
    USHORT         ProcessNameLength;
    WCHAR          ProcessName[260];
    USHORT         FileNameLength;
    WCHAR          FileName[512];
    USHORT         OriginalFileNameLength;
    WCHAR          OriginalFileName[512];
    USHORT         QuarantineFileNameLength;
    WCHAR          QuarantineFileName[512];
    ULONG          FileSize;
    ACTION_RESULT  ActionResult;
    ULONG          Reserved;
} DLP_FILE_EVENT, *PDLP_FILE_EVENT;

typedef struct _DLP_NET_EVENT {
    DLP_EVENT_TYPE EventType;
    LARGE_INTEGER  Timestamp;
    ULONG          ProcessId;
    USHORT         ProcessNameLength;
    WCHAR          ProcessName[256];
    USHORT         RemoteAddressLength;
    UCHAR          RemoteAddress[128];
    USHORT         LocalAddressLength;
    UCHAR          LocalAddress[128];
    USHORT         UrlLength;
    WCHAR          Url[1024];
    USHORT         SniDomainLength;
    WCHAR          SniDomain[256];
    USHORT         ContentTypeLength;
    WCHAR          ContentType[64];
    USHORT         ContentEncodingLength;
    WCHAR          ContentEncoding[32];
    USHORT         BodyPreviewLength;
    UCHAR          BodyPreview[2048];
    UINT8          Protocol;
    UINT16         RemotePort;
    UINT16         LocalPort;
    ACTION_RESULT  ActionResult;
    ULONG          Reserved;
} DLP_NET_EVENT, *PDLP_NET_EVENT;

#define PS_FILE_BATCH_MAX 8
#define PS_NET_BATCH_MAX 4

typedef struct _DLP_FILE_EVENT_BATCH {
    ULONG Count;
    ULONG Reserved;
    DLP_FILE_EVENT Events[PS_FILE_BATCH_MAX];
} DLP_FILE_EVENT_BATCH, *PDLP_FILE_EVENT_BATCH;

typedef struct _DLP_NET_EVENT_BATCH {
    ULONG Count;
    ULONG Reserved;
    DLP_NET_EVENT Events[PS_NET_BATCH_MAX];
} DLP_NET_EVENT_BATCH, *PDLP_NET_EVENT_BATCH;

#define PS_EVENT_FILE_READY 0x00000001UL
#define PS_EVENT_NET_READY  0x00000002UL

typedef struct _PS_EVENT_WAIT_REQUEST {
    ULONG TimeoutMs;
    ULONG ReadyMask;
} PS_EVENT_WAIT_REQUEST, *PPS_EVENT_WAIT_REQUEST;

#define PS_POLICY_DATA_CAPACITY 16384

typedef struct _POLICY_COMMAND {
    ULONG CommandId;
    ULONG PolicySize;
    UCHAR PolicyData[PS_POLICY_DATA_CAPACITY];
} POLICY_COMMAND, *PPOLICY_COMMAND;

#define PS_QUARANTINE_ROOT_LEN 260

typedef struct _PS_QUARANTINE_SETTINGS {
    ULONG Enabled;
    WCHAR RootDirectory[PS_QUARANTINE_ROOT_LEN];
} PS_QUARANTINE_SETTINGS, *PPS_QUARANTINE_SETTINGS;

#define PS_REDIRECT_ADDR_MAX 16

typedef struct _PS_REDIRECT_SETTINGS {
    ULONG Enabled;
    ULONG ProxyProcessId;
    UINT16 ProxyPort;
    UINT8 AddressFamily;
    UINT8 Reserved0;
    UCHAR ProxyAddress[PS_REDIRECT_ADDR_MAX];
} PS_REDIRECT_SETTINGS, *PPS_REDIRECT_SETTINGS;

typedef struct _PS_REDIRECT_QUERY {
    ULONG ProcessId;
    UINT8 Protocol;
    UINT8 AddressFamily;
    UINT16 LocalPort;
    UCHAR LocalAddress[PS_REDIRECT_ADDR_MAX];
    ULONG Found;
    UINT16 OriginalRemotePort;
    UCHAR OriginalRemoteAddress[PS_REDIRECT_ADDR_MAX];
} PS_REDIRECT_QUERY, *PPS_REDIRECT_QUERY;

#define PS_PROTECT_VERSION 1UL
#define PS_PROTECT_NONCE_LEN 32
#define PS_PROTECT_BOOT_ID_LEN 16
#define PS_PROTECT_SIGNATURE_LEN 256
#define PS_PROTECT_MAX_TTL_SECONDS 300UL
#define PS_PROTECT_MAX_PATHS 8
#define PS_PROTECT_PATH_LEN 260
#define PS_PROTECT_MAX_PIDS 16
#define PS_PROTECT_MAX_PROCESS_IMAGES 8

#pragma pack(push, 1)
typedef struct _PS_PROTECT_TICKET {
    ULONG Version;
    ULONG TtlSeconds;
    ULONG RequestorPid;
    ULONG Reserved;
    ULONGLONG ChallengeIssued;
    UCHAR BootId[PS_PROTECT_BOOT_ID_LEN];
    UCHAR Nonce[PS_PROTECT_NONCE_LEN];
} PS_PROTECT_TICKET, *PPS_PROTECT_TICKET;

typedef struct _PS_PROTECT_UNLOCK_REQUEST {
    PS_PROTECT_TICKET Ticket;
    ULONG SignatureLength;
    UCHAR Signature[PS_PROTECT_SIGNATURE_LEN];
} PS_PROTECT_UNLOCK_REQUEST, *PPS_PROTECT_UNLOCK_REQUEST;
#pragma pack(pop)

typedef struct _PS_PROTECT_LIST {
    ULONG Enabled;
    ULONG PathCount;
    ULONG PidCount;
    ULONG ProcessImageCount;
    WCHAR Paths[PS_PROTECT_MAX_PATHS][PS_PROTECT_PATH_LEN];
    ULONG Pids[PS_PROTECT_MAX_PIDS];
    WCHAR ProcessImages[PS_PROTECT_MAX_PROCESS_IMAGES][PS_PROTECT_PATH_LEN];
} PS_PROTECT_LIST, *PPS_PROTECT_LIST;

typedef struct _PS_PROTECT_STATUS {
    ULONG Version;
    ULONG Enabled;
    ULONG Mode;
    ULONG RemainingTtlSeconds;
    ULONG ChallengeActive;
    ULONG PathCount;
    ULONG PidCount;
    ULONG FailedUnlocks;
    ULONG ProcessImageCount;
} PS_PROTECT_STATUS, *PPS_PROTECT_STATUS;

static_assert(sizeof(PS_PROTECT_TICKET) == 72, "PS_PROTECT_TICKET ABI mismatch");
static_assert(sizeof(PS_PROTECT_UNLOCK_REQUEST) == 332, "PS_PROTECT_UNLOCK_REQUEST ABI mismatch");
static_assert(sizeof(PS_PROTECT_LIST) == 8400, "PS_PROTECT_LIST ABI mismatch");

#define PS_PROTECT_CONTROL_TOKEN_LEN 32
#define PS_PROTECT_CONTROL_INITIALIZE 1UL
#define PS_PROTECT_CONTROL_UNLOCK 2UL
#define PS_PROTECT_CONTROL_RELOCK 3UL

typedef struct _PS_PROTECT_CONTROL {
    ULONG Version;
    ULONG Action;
    ULONG ProcessId;
    ULONG Reserved;
    UCHAR Token[PS_PROTECT_CONTROL_TOKEN_LEN];
    WCHAR InstallDirectory[PS_PROTECT_PATH_LEN];
    WCHAR ProcessImage[PS_PROTECT_PATH_LEN];
    WCHAR DriverPath[PS_PROTECT_PATH_LEN];
} PS_PROTECT_CONTROL, *PPS_PROTECT_CONTROL;

#define PS_MAX_BLOCK_EXT    16
#define PS_MAX_BLOCK_PROC   16
#define PS_MAX_BLOCK_PORT   32
#define PS_MAX_BLOCK_DOMAIN 8
#define PS_MAX_BLOCK_URL    4
#define PS_MAX_BLOCK_FTP_CMD 8
#define PS_MAX_BLOCK_FTP_PATH 8
#define PS_MAX_BLOCK_FTP_CONTENT 8
#define PS_MAX_BLOCK_HTTP_HEADER 8
#define PS_MAX_BLOCK_HTTP_TRAILER 8
#define PS_MAX_BLOCK_HTTP_BODY 8
#define PS_MAX_BLOCK_JSON_KEY 8
#define PS_MAX_BLOCK_JSON_PATH 8
#define PS_MAX_BLOCK_JSON_VALUE 8
#define PS_EXT_LEN          16
#define PS_PROC_LEN         64
#define PS_DOMAIN_LEN       64
#define PS_URL_LEN          48
#define PS_FTP_CMD_LEN      16
#define PS_FTP_PATH_LEN     64
#define PS_FTP_CONTENT_LEN  64
#define PS_HTTP_HEADER_LEN  48
#define PS_HTTP_BODY_LEN    64
#define PS_JSON_KEY_LEN     32
#define PS_JSON_PATH_LEN    64
#define PS_JSON_VALUE_LEN   48

typedef struct _PS_POLICY_DATA {
    ULONG  FileFilterEnabled;
    ULONG  NetworkFilterEnabled;
    ULONG  AuditEnabled;
    ULONG  BlockedExtCount;
    WCHAR  BlockedExtensions[PS_MAX_BLOCK_EXT][PS_EXT_LEN];
    ULONG  BlockedProcCount;
    WCHAR  BlockedProcesses[PS_MAX_BLOCK_PROC][PS_PROC_LEN];
    ULONG  BlockedPortCount;
    UINT16 BlockedPorts[PS_MAX_BLOCK_PORT];
    ULONG  BlockedDomainCount;
    WCHAR  BlockedDomains[PS_MAX_BLOCK_DOMAIN][PS_DOMAIN_LEN];
    ULONG  BlockedUrlCount;
    WCHAR  BlockedUrls[PS_MAX_BLOCK_URL][PS_URL_LEN];
    ULONG  BlockedFtpCommandCount;
    WCHAR  BlockedFtpCommands[PS_MAX_BLOCK_FTP_CMD][PS_FTP_CMD_LEN];
    ULONG  BlockedFtpPathCount;
    WCHAR  BlockedFtpPaths[PS_MAX_BLOCK_FTP_PATH][PS_FTP_PATH_LEN];
    ULONG  BlockedFtpContentPatternCount;
    WCHAR  BlockedFtpContentPatterns[PS_MAX_BLOCK_FTP_CONTENT][PS_FTP_CONTENT_LEN];
    ULONG  BlockedHttpHeaderCount;
    WCHAR  BlockedHttpHeaders[PS_MAX_BLOCK_HTTP_HEADER][PS_HTTP_HEADER_LEN];
    ULONG  BlockedHttpTrailerCount;
    WCHAR  BlockedHttpTrailers[PS_MAX_BLOCK_HTTP_TRAILER][PS_HTTP_HEADER_LEN];
    ULONG  BlockedHttpBodyPatternCount;
    WCHAR  BlockedHttpBodyPatterns[PS_MAX_BLOCK_HTTP_BODY][PS_HTTP_BODY_LEN];
    ULONG  BlockedJsonKeyCount;
    WCHAR  BlockedJsonKeys[PS_MAX_BLOCK_JSON_KEY][PS_JSON_KEY_LEN];
    ULONG  BlockedJsonPathCount;
    WCHAR  BlockedJsonPaths[PS_MAX_BLOCK_JSON_PATH][PS_JSON_PATH_LEN];
    ULONG  BlockedJsonValueCount;
    WCHAR  BlockedJsonValues[PS_MAX_BLOCK_JSON_VALUE][PS_JSON_VALUE_LEN];
} PS_POLICY_DATA, *PPS_POLICY_DATA;

static_assert(sizeof(PS_POLICY_DATA) <= PS_POLICY_DATA_CAPACITY, "PS_POLICY_DATA exceeds IOCTL payload capacity");

typedef struct _DRIVER_STATUS {
    BOOLEAN       DriverLoaded;
    BOOLEAN       FileFilterActive;
    BOOLEAN       NetworkFilterActive;
    ULONG         LastErrorCode;
    LARGE_INTEGER StartTime;
    ULONG         TotalEvents;
    ULONG         BlockedEvents;
    ULONG         FileQueueDepth;
    ULONG         NetQueueDepth;
    ULONG         FileDroppedEvents;
    ULONG         NetDroppedEvents;
} DRIVER_STATUS, *PDRIVER_STATUS;

#define CMD_SET_POLICY  0
#define CMD_GET_POLICY  1
