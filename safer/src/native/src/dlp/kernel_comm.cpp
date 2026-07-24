/*
 * kernel_comm.cpp - 内核通信实现
 * 通过 IOCTL 和命名管道与 PersonalSafer.sys 通信
 */

#include "kernel_comm.h"
#include "ps_shared.h"
#include "../common/utils.h"
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <cstddef>
#include <string>
#include <vector>

static KernelCommState g_commState;

// 设备符号链接路径：\\.\PersonalSafer
static const wchar_t* kDevicePath = L"\\\\.\\PersonalSafer";
static const wchar_t* kPipePath = L"\\\\.\\pipe\\PersonalSafer\\Control";
static const LONGLONG kUnixEpochInFileTime = 116444736000000000LL;

// 事件类型枚举 -> 字符串
static const char* EventTypeToStr(DLP_EVENT_TYPE t) {
    switch (t) {
        case EventFileCreate: return "file_create";
        case EventFileWrite:  return "file_write";
        case EventFileRead:   return "file_read";
        case EventFileDelete: return "file_delete";
        case EventFileRename: return "file_rename";
        case EventNetworkConnect:    return "network_connect";
        case EventNetworkDisconnect: return "network_disconnect";
        case EventHttpRequest:  return "http_request";
        case EventHttpResponse: return "http_response";
        case EventFtpCommand:   return "ftp_command";
        case EventRegistryChange: return "registry_change";
        case EventSniCapture:   return "sni_capture";
        default: return "unknown";
    }
}

static const char* ActionToStr(ACTION_RESULT a) {
    switch (a) {
        case ActionAllowed: return "allowed";
        case ActionBlocked: return "blocked";
        case ActionLogged:  return "logged";
        case ActionQuarantined: return "quarantined";
        default: return "logged";
    }
}

// 防御：把宽字符长度截断到首个 NUL 之内（不超过 maxLen），避免越界读
static size_t ClampWLen(const WCHAR* buf, size_t maxLen) {
    size_t n = 0;
    while (n < maxLen && buf[n] != L'\0') n++;
    return n;
}

// 防御：把 ASCII 长度截断到首个 NUL 之内（不超过 maxLen）
static size_t ClampALen(const char* buf, size_t maxLen) {
    size_t n = 0;
    while (n < maxLen && buf[n] != '\0') n++;
    return n;
}

static double KernelTimeToUnixMillis(const LARGE_INTEGER& ts) {
    if (ts.QuadPart <= kUnixEpochInFileTime) {
        return 0;
    }
    return static_cast<double>((ts.QuadPart - kUnixEpochInFileTime) / 10000);
}

struct DeviceDriveMapping {
    std::wstring devicePath;
    std::wstring driveName;
};

static std::vector<DeviceDriveMapping> BuildDeviceDriveMappings() {
    std::vector<DeviceDriveMapping> mappings;
    DWORD chars = GetLogicalDriveStringsW(0, nullptr);
    if (chars == 0) return mappings;

    std::vector<WCHAR> drives(chars + 1, L'\0');
    if (GetLogicalDriveStringsW(chars, drives.data()) == 0) return mappings;

    for (const WCHAR* drive = drives.data(); *drive != L'\0'; drive += wcslen(drive) + 1) {
        if (wcslen(drive) < 2 || drive[1] != L':') continue;

        WCHAR driveName[] = { drive[0], L':', L'\0' };
        std::vector<WCHAR> targets(32768, L'\0');
        if (QueryDosDeviceW(driveName, targets.data(), static_cast<DWORD>(targets.size())) == 0) {
            continue;
        }

        for (const WCHAR* target = targets.data(); *target != L'\0'; target += wcslen(target) + 1) {
            mappings.push_back({ target, driveName });
        }
    }
    return mappings;
}

static std::wstring NormalizeFilePath(const std::wstring& input) {
    if (input.empty()) return input;

    if (_wcsnicmp(input.c_str(), L"\\??\\UNC\\", 8) == 0) {
        return L"\\\\" + input.substr(8);
    }
    if (_wcsnicmp(input.c_str(), L"\\??\\", 4) == 0) {
        return input.substr(4);
    }
    if (_wcsnicmp(input.c_str(), L"\\\\?\\UNC\\", 8) == 0) {
        return L"\\\\" + input.substr(8);
    }
    if (_wcsnicmp(input.c_str(), L"\\\\?\\", 4) == 0) {
        return input.substr(4);
    }
    if (_wcsnicmp(input.c_str(), L"\\Device\\Mup\\", 12) == 0) {
        return L"\\\\" + input.substr(12);
    }
    if (_wcsnicmp(input.c_str(), L"\\SystemRoot\\", 12) == 0) {
        std::vector<WCHAR> windowsDirectory(MAX_PATH + 1, L'\0');
        UINT length = GetWindowsDirectoryW(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
        if (length > 0 && length < windowsDirectory.size()) {
            return std::wstring(windowsDirectory.data(), length) + input.substr(11);
        }
    }

    static const std::vector<DeviceDriveMapping> mappings = BuildDeviceDriveMappings();
    for (const auto& mapping : mappings) {
        const size_t prefixLength = mapping.devicePath.size();
        if (input.size() < prefixLength ||
            _wcsnicmp(input.c_str(), mapping.devicePath.c_str(), prefixLength) != 0) {
            continue;
        }
        if (input.size() > prefixLength && input[prefixLength] != L'\\') {
            continue;
        }
        return mapping.driveName + input.substr(prefixLength);
    }

    return input;
}

static std::wstring ToKernelDevicePath(const std::wstring& input) {
    if (input.empty() || _wcsnicmp(input.c_str(), L"\\Device\\", 8) == 0) return input;

    std::wstring normalized = input;
    if (_wcsnicmp(normalized.c_str(), L"\\\\?\\", 4) == 0 ||
        _wcsnicmp(normalized.c_str(), L"\\??\\", 4) == 0) {
        normalized = normalized.substr(4);
    }
    if (normalized.size() >= 3 && normalized[1] == L':' && normalized[2] == L'\\') {
        static const std::vector<DeviceDriveMapping> mappings = BuildDeviceDriveMappings();
        WCHAR driveName[] = { normalized[0], L':', L'\0' };
        for (const auto& mapping : mappings) {
            if (_wcsicmp(mapping.driveName.c_str(), driveName) == 0) {
                return mapping.devicePath + normalized.substr(2);
            }
        }
    }
    if (_wcsnicmp(normalized.c_str(), L"\\\\", 2) == 0) {
        return L"\\Device\\Mup" + normalized.substr(1);
    }
    return normalized;
}

static const UCHAR kProtectControlToken[PS_PROTECT_CONTROL_TOKEN_LEN] = {
    0x50,0x53,0x2D,0x42,0x4F,0x4F,0x54,0x2D,0x32,0x30,0x32,0x36,0x2D,0x56,0x31,0x2D,
    0x71,0x37,0x4B,0x39,0x6D,0x32,0x52,0x34,0x78,0x38,0x54,0x33,0x63,0x35,0x4E,0x21
};

static bool CopyProtectPath(WCHAR* target, size_t targetChars, const std::wstring& value) {
    if (value.empty() || value.size() >= targetChars || value[0] != L'\\') return false;
    wmemcpy(target, value.c_str(), value.size());
    target[value.size()] = L'\0';
    return true;
}

static bool SendProtectControl(HANDLE device, ULONG action) {
    PS_PROTECT_CONTROL control = {};
    DWORD bytesReturned = 0;
    control.Version = PS_PROTECT_VERSION;
    control.Action = action;
    memcpy(control.Token, kProtectControlToken, sizeof(control.Token));

    if (action == PS_PROTECT_CONTROL_INITIALIZE) {
        std::vector<WCHAR> modulePath(32768, L'\0');
        std::vector<WCHAR> windowsPath(32768, L'\0');
        DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        UINT windowsLength = GetWindowsDirectoryW(windowsPath.data(), static_cast<UINT>(windowsPath.size()));
        if (moduleLength == 0 || moduleLength >= modulePath.size() ||
            windowsLength == 0 || windowsLength >= windowsPath.size()) return false;

        std::wstring image(modulePath.data(), moduleLength);
        size_t separator = image.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return false;
        std::wstring installDirectory = image.substr(0, separator);
        std::wstring driverPath(windowsPath.data(), windowsLength);
        driverPath += L"\\System32\\drivers\\PersonalSafer.sys";
        control.ProcessId = GetCurrentProcessId();
        if (!CopyProtectPath(control.InstallDirectory, ARRAYSIZE(control.InstallDirectory),
                ToKernelDevicePath(installDirectory)) ||
            !CopyProtectPath(control.ProcessImage, ARRAYSIZE(control.ProcessImage),
                ToKernelDevicePath(image)) ||
            !CopyProtectPath(control.DriverPath, ARRAYSIZE(control.DriverPath),
                ToKernelDevicePath(driverPath))) return false;
    }

    BOOL ok = DeviceIoControl(device, IOCTL_PS_PROTECT_CONTROL, &control, sizeof(control),
        nullptr, 0, &bytesReturned, nullptr);
    SecureZeroMemory(&control, sizeof(control));
    return ok != FALSE;
}

static Napi::String WideStringToNapi(Napi::Env env, const std::wstring& value) {
    return Napi::String::New(
        env,
        reinterpret_cast<const char16_t*>(value.data()),
        value.size());
}

static Napi::Value NormalizeFilePathForJs(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsString()) {
        return Napi::String::New(env, "");
    }

    const std::u16string input = info[0].As<Napi::String>().Utf16Value();
    const std::wstring path(
        reinterpret_cast<const WCHAR*>(input.data()),
        input.size());
    return WideStringToNapi(env, NormalizeFilePath(path));
}

class WaitForEventsWorker : public Napi::AsyncWorker {
public:
    WaitForEventsWorker(Napi::Env env, ULONG timeoutMs)
        : Napi::AsyncWorker(env),
          deferred(Napi::Promise::Deferred::New(env)),
          timeoutMs_(timeoutMs),
          readyMask_(0),
          ok_(FALSE),
          bytesReturned_(0) {}

    void Execute() override {
        HANDLE waitHandle = CreateFileW(
            kDevicePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        PS_EVENT_WAIT_REQUEST request = {};

        if (waitHandle == INVALID_HANDLE_VALUE) {
            return;
        }
        request.TimeoutMs = timeoutMs_;
        ok_ = DeviceIoControl(
            waitHandle,
            IOCTL_PS_WAIT_EVENTS,
            &request,
            sizeof(request),
            &request,
            sizeof(request),
            &bytesReturned_,
            NULL);
        CloseHandle(waitHandle);
        if (ok_ && bytesReturned_ >= sizeof(request)) {
            readyMask_ = request.ReadyMask;
        }
    }

    void OnOK() override {
        Napi::Object result = Napi::Object::New(Env());
        result.Set("readyMask", Napi::Number::New(Env(), readyMask_));
        result.Set("timedOut", Napi::Boolean::New(Env(), readyMask_ == 0));
        deferred.Resolve(result);
    }

    void OnError(const Napi::Error&) override {
        Napi::Object result = Napi::Object::New(Env());
        result.Set("readyMask", Napi::Number::New(Env(), 0));
        result.Set("timedOut", Napi::Boolean::New(Env(), true));
        deferred.Resolve(result);
    }

    Napi::Promise Promise() { return deferred.Promise(); }

private:
    Napi::Promise::Deferred deferred;
    ULONG timeoutMs_;
    ULONG readyMask_;
    BOOL ok_;
    DWORD bytesReturned_;
};

static Napi::Object BuildFileEventObject(Napi::Env env, const DLP_FILE_EVENT& ev) {
    Napi::Object o = Napi::Object::New(env);
    size_t pnLen = ev.ProcessNameLength; if (pnLen > 259) pnLen = 259;
    pnLen = ClampWLen(ev.ProcessName, pnLen);
    size_t fnLen = ev.FileNameLength; if (fnLen > 511) fnLen = 511;
    fnLen = ClampWLen(ev.FileName, fnLen);
    size_t ofnLen = ev.OriginalFileNameLength; if (ofnLen > 511) ofnLen = 511;
    ofnLen = ClampWLen(ev.OriginalFileName, ofnLen);
    size_t qfnLen = ev.QuarantineFileNameLength; if (qfnLen > 511) qfnLen = 511;
    qfnLen = ClampWLen(ev.QuarantineFileName, qfnLen);
    const std::wstring rawFileName(ev.FileName, fnLen);
    const std::wstring rawOriginalFileName(ev.OriginalFileName, ofnLen);
    const std::wstring rawQuarantineFileName(ev.QuarantineFileName, qfnLen);
    o.Set("type", Napi::String::New(env, EventTypeToStr(ev.EventType)));
    o.Set("processId", Napi::Number::New(env, ev.ProcessId));
    o.Set("processName", Napi::String::New(env,
        reinterpret_cast<const char16_t*>(ev.ProcessName), pnLen));
    o.Set("fileName", WideStringToNapi(env, NormalizeFilePath(rawFileName)));
    o.Set("originalFileName", WideStringToNapi(env, NormalizeFilePath(rawOriginalFileName)));
    o.Set("quarantineFileName", WideStringToNapi(env, NormalizeFilePath(rawQuarantineFileName)));
    o.Set("rawFileName", WideStringToNapi(env, rawFileName));
    o.Set("rawOriginalFileName", WideStringToNapi(env, rawOriginalFileName));
    o.Set("rawQuarantineFileName", WideStringToNapi(env, rawQuarantineFileName));
    o.Set("fileSize", Napi::Number::New(env, ev.FileSize));
    o.Set("action", Napi::String::New(env, ActionToStr(ev.ActionResult)));
    o.Set("timestamp", Napi::Number::New(env, static_cast<double>(ev.Timestamp.QuadPart)));
    o.Set("timestampMs", Napi::Number::New(env, KernelTimeToUnixMillis(ev.Timestamp)));
    return o;
}

static Napi::Object BuildNetEventObject(Napi::Env env, const DLP_NET_EVENT& ev) {
    Napi::Object o = Napi::Object::New(env);
    size_t pnLen = ev.ProcessNameLength; if (pnLen > 255) pnLen = 255;
    pnLen = ClampWLen(ev.ProcessName, pnLen);
    size_t raLen = ev.RemoteAddressLength; if (raLen > 127) raLen = 127;
    raLen = ClampALen(reinterpret_cast<const char*>(ev.RemoteAddress), raLen);
    size_t laLen = ev.LocalAddressLength; if (laLen > 127) laLen = 127;
    laLen = ClampALen(reinterpret_cast<const char*>(ev.LocalAddress), laLen);
    size_t urlLen = ev.UrlLength; if (urlLen > 1023) urlLen = 1023;
    urlLen = ClampWLen(ev.Url, urlLen);
    size_t sniLen = ev.SniDomainLength; if (sniLen > 255) sniLen = 255;
    sniLen = ClampWLen(ev.SniDomain, sniLen);
    size_t ctLen = ev.ContentTypeLength; if (ctLen > 63) ctLen = 63;
    ctLen = ClampWLen(ev.ContentType, ctLen);
    size_t ceLen = ev.ContentEncodingLength; if (ceLen > 31) ceLen = 31;
    ceLen = ClampWLen(ev.ContentEncoding, ceLen);
    size_t bpLen = ev.BodyPreviewLength; if (bpLen > 2048) bpLen = 2048;
    o.Set("type", Napi::String::New(env, EventTypeToStr(ev.EventType)));
    o.Set("processId", Napi::Number::New(env, ev.ProcessId));
    o.Set("processName", Napi::String::New(env,
        reinterpret_cast<const char16_t*>(ev.ProcessName), pnLen));
    o.Set("remoteAddress", Napi::String::New(env,
        reinterpret_cast<const char*>(ev.RemoteAddress), raLen));
    o.Set("localAddress", Napi::String::New(env,
        reinterpret_cast<const char*>(ev.LocalAddress), laLen));
    o.Set("url", Napi::String::New(env,
        reinterpret_cast<const char16_t*>(ev.Url), urlLen));
    o.Set("sniDomain", Napi::String::New(env,
        reinterpret_cast<const char16_t*>(ev.SniDomain), sniLen));
    o.Set("contentType", Napi::String::New(env,
        reinterpret_cast<const char16_t*>(ev.ContentType), ctLen));
    o.Set("contentEncoding", Napi::String::New(env,
        reinterpret_cast<const char16_t*>(ev.ContentEncoding), ceLen));
    o.Set("bodyPreview", Napi::Buffer<UCHAR>::Copy(env, ev.BodyPreview, bpLen));
    o.Set("remotePort", Napi::Number::New(env, ev.RemotePort));
    o.Set("localPort", Napi::Number::New(env, ev.LocalPort));
    o.Set("protocol", Napi::Number::New(env, ev.Protocol));
    o.Set("action", Napi::String::New(env, ActionToStr(ev.ActionResult)));
    o.Set("timestamp", Napi::Number::New(env, static_cast<double>(ev.Timestamp.QuadPart)));
    o.Set("timestampMs", Napi::Number::New(env, KernelTimeToUnixMillis(ev.Timestamp)));
    return o;
}

static bool ParseIpv4Address(const std::string& text, DWORD* outAddr) {
    IN_ADDR addr = {};
    if (outAddr == nullptr) return false;
    if (InetPtonA(AF_INET, text.c_str(), &addr) != 1) return false;
    *outAddr = addr.S_un.S_addr;
    return true;
}

static bool ParseIpv6Address(const std::string& text, BYTE outAddr[16]) {
    IN6_ADDR addr = {};
    if (outAddr == nullptr) return false;
    if (InetPtonA(AF_INET6, text.c_str(), &addr) != 1) return false;
    memcpy(outAddr, &addr, 16);
    return true;
}

static USHORT TcpRowPortToHostOrder(DWORD rawPort) {
    return ntohs(*reinterpret_cast<USHORT*>(&rawPort));
}

static DWORD FindTcpOwnerPidV4(
    const std::string& localAddress,
    USHORT localPort,
    const std::string& remoteAddress,
    USHORT remotePort
) {
    DWORD tableSize = 0;
    DWORD localAddr = 0;
    DWORD remoteAddr = 0;
    PMIB_TCPTABLE_OWNER_PID table = nullptr;
    DWORD pid = 0;

    if (!ParseIpv4Address(localAddress, &localAddr) ||
        !ParseIpv4Address(remoteAddress, &remoteAddr)) {
        return 0;
    }

    GetExtendedTcpTable(nullptr, &tableSize, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(malloc(tableSize));
    if (table == nullptr) return 0;

    if (GetExtendedTcpTable(table, &tableSize, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const MIB_TCPROW_OWNER_PID& row = table->table[i];
            if (row.dwLocalAddr == localAddr &&
                row.dwRemoteAddr == remoteAddr &&
                TcpRowPortToHostOrder(row.dwLocalPort) == localPort &&
                TcpRowPortToHostOrder(row.dwRemotePort) == remotePort) {
                pid = row.dwOwningPid;
                break;
            }
        }
    }

    free(table);
    return pid;
}

static DWORD FindTcpOwnerPidV6(
    const std::string& localAddress,
    USHORT localPort,
    const std::string& remoteAddress,
    USHORT remotePort
) {
    DWORD tableSize = 0;
    BYTE localAddr[16] = {};
    BYTE remoteAddr[16] = {};
    PMIB_TCP6TABLE_OWNER_PID table = nullptr;
    DWORD pid = 0;

    if (!ParseIpv6Address(localAddress, localAddr) ||
        !ParseIpv6Address(remoteAddress, remoteAddr)) {
        return 0;
    }

    GetExtendedTcpTable(nullptr, &tableSize, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    table = reinterpret_cast<PMIB_TCP6TABLE_OWNER_PID>(malloc(tableSize));
    if (table == nullptr) return 0;

    if (GetExtendedTcpTable(table, &tableSize, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const MIB_TCP6ROW_OWNER_PID& row = table->table[i];
            if (memcmp(row.ucLocalAddr, localAddr, 16) == 0 &&
                memcmp(row.ucRemoteAddr, remoteAddr, 16) == 0 &&
                TcpRowPortToHostOrder(row.dwLocalPort) == localPort &&
                TcpRowPortToHostOrder(row.dwRemotePort) == remotePort) {
                pid = row.dwOwningPid;
                break;
            }
        }
    }

    free(table);
    return pid;
}

/*++
 * ConnectToDevice
 *
 * 连接到内核驱动设备
 *
 * --*/
Napi::Value ConnectToDevice(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (g_commState.isConnected && g_commState.deviceHandle != INVALID_HANDLE_VALUE) {
        DWORD bytesReturned = 0;
        if (DeviceIoControl(
                g_commState.deviceHandle,
                IOCTL_PS_HEARTBEAT,
                nullptr,
                0,
                nullptr,
                0,
                &bytesReturned,
                nullptr)) {
            return Napi::Boolean::New(env, true);
        }

        CloseHandle(g_commState.deviceHandle);
        g_commState.deviceHandle = INVALID_HANDLE_VALUE;
        g_commState.isConnected = false;
        g_commState.canControl = false;
    }

    g_commState.deviceHandle = CreateFileW(
        kDevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (g_commState.deviceHandle == INVALID_HANDLE_VALUE) {
        const DWORD readWriteError = GetLastError();
        g_commState.deviceHandle = CreateFileW(
            kDevicePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (g_commState.deviceHandle == INVALID_HANDLE_VALUE) {
            const DWORD readOnlyError = GetLastError();
            g_commState.lastErrorCode = readOnlyError;
            g_commState.lastErrorStage = "open_device";
            g_commState.lastError =
                "CreateFile(\\\\.\\PersonalSafer) failed (read/write=" +
                std::to_string(readWriteError) + ", read-only=" +
                std::to_string(readOnlyError) + ")";
            return Napi::Boolean::New(env, false);
        }
        g_commState.canControl = false;
    } else {
        g_commState.canControl = true;
    }

    g_commState.isConnected = true;
    g_commState.lastErrorCode = ERROR_SUCCESS;
    g_commState.lastErrorStage.clear();
    g_commState.lastError.clear();
    return Napi::Boolean::New(env, true);
}

Napi::Value GetConnectionState(const Napi::CallbackInfo& info) {
    Napi::Object state = Napi::Object::New(info.Env());
    state.Set("connected", Napi::Boolean::New(info.Env(), g_commState.isConnected));
    state.Set("canControl", Napi::Boolean::New(info.Env(), g_commState.canControl));
    state.Set("lastErrorCode", Napi::Number::New(info.Env(), g_commState.lastErrorCode));
    state.Set("lastErrorStage", Napi::String::New(info.Env(), g_commState.lastErrorStage));
    state.Set("lastError", Napi::String::New(info.Env(), g_commState.lastError));
    return state;
}

/*++
 * DisconnectFromDevice
 *
 * 断开与内核驱动的连接
 *
 * --*/
Napi::Value DisconnectFromDevice(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (g_commState.deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_commState.deviceHandle);
        g_commState.deviceHandle = INVALID_HANDLE_VALUE;
    }
    if (g_commState.pipeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_commState.pipeHandle);
        g_commState.pipeHandle = INVALID_HANDLE_VALUE;
    }
    g_commState.isConnected = false;
    g_commState.canControl = false;
    g_commState.isPipeConnected = false;

    return Napi::Boolean::New(env, true);
}

/*++
 * SendIoctl
 *
 * 发送 IOCTL 命令到驱动
 *
 * --*/
Napi::Value SendIoctl(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_commState.isConnected) {
        return Napi::Boolean::New(env, false);
    }

    // 获取参数
    uint32_t ioctlCode = info[0].As<Napi::Number>().Uint32Value();
    std::string inputData = info[1].As<Napi::String>().Utf8Value();

    DWORD bytesReturned = 0;
    BOOL result = DeviceIoControl(
        g_commState.deviceHandle,
        ioctlCode,
        inputData.empty() ? nullptr : const_cast<char*>(inputData.c_str()),
        static_cast<DWORD>(inputData.size()),
        NULL,
        0,
        &bytesReturned,
        NULL
    );

    Napi::Object ret = Napi::Object::New(env);
    ret.Set("success", Napi::Boolean::New(env, result != 0));
    ret.Set("bytesReturned", Napi::Number::New(env, bytesReturned));
    return ret;
}

Napi::Value GetDriverStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_commState.isConnected) {
        return env.Null();
    }

    DRIVER_STATUS status = {};
    DWORD bytesReturned = 0;
    BOOL result = DeviceIoControl(
        g_commState.deviceHandle,
        IOCTL_PS_DRIVER_STATUS,
        NULL,
        0,
        &status,
        sizeof(status),
        &bytesReturned,
        NULL);

    const DWORD minimumStatusBytes =
        static_cast<DWORD>(offsetof(DRIVER_STATUS, NetworkFilterActive) + sizeof(status.NetworkFilterActive));
    if (!result || bytesReturned < minimumStatusBytes) {
        return env.Null();
    }

    Napi::Object o = Napi::Object::New(env);
    o.Set("driverLoaded", Napi::Boolean::New(env, status.DriverLoaded != FALSE));
    o.Set("fileFilterActive", Napi::Boolean::New(env, status.FileFilterActive != FALSE));
    o.Set("networkFilterActive", Napi::Boolean::New(env, status.NetworkFilterActive != FALSE));
    o.Set("lastErrorCode", Napi::Number::New(env, status.LastErrorCode));
    o.Set("startTime", Napi::Number::New(env, static_cast<double>(status.StartTime.QuadPart)));
    o.Set("totalEvents", Napi::Number::New(env, status.TotalEvents));
    o.Set("blockedEvents", Napi::Number::New(env, status.BlockedEvents));
    o.Set("fileQueueDepth", Napi::Number::New(env, status.FileQueueDepth));
    o.Set("netQueueDepth", Napi::Number::New(env, status.NetQueueDepth));
    o.Set("fileDroppedEvents", Napi::Number::New(env, status.FileDroppedEvents));
    o.Set("netDroppedEvents", Napi::Number::New(env, status.NetDroppedEvents));
    return o;
}

Napi::Value GetProtectionChallenge(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    PS_PROTECT_TICKET ticket = {};
    DWORD bytesReturned = 0;
    ULONG ttlSeconds = PS_PROTECT_MAX_TTL_SECONDS;

    if (!g_commState.isConnected) return env.Null();
    if (info.Length() >= 1 && info[0].IsNumber()) {
        ttlSeconds = info[0].As<Napi::Number>().Uint32Value();
    }
    if (ttlSeconds == 0 || ttlSeconds > PS_PROTECT_MAX_TTL_SECONDS) {
        Napi::RangeError::New(env, "protection TTL must be between 1 and 300 seconds")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    BOOL ok = DeviceIoControl(
        g_commState.deviceHandle,
        IOCTL_PS_GET_PROTECT_CHALLENGE,
        nullptr,
        0,
        &ticket,
        sizeof(ticket),
        &bytesReturned,
        nullptr);
    if (!ok || bytesReturned != sizeof(ticket)) return env.Null();

    ticket.TtlSeconds = ttlSeconds;
    Napi::Object result = Napi::Object::New(env);
    result.Set("ticket", Napi::Buffer<UCHAR>::Copy(
        env, reinterpret_cast<const UCHAR*>(&ticket), sizeof(ticket)));
    result.Set("version", Napi::Number::New(env, ticket.Version));
    result.Set("ttlSeconds", Napi::Number::New(env, ticket.TtlSeconds));
    result.Set("requestorPid", Napi::Number::New(env, ticket.RequestorPid));
    result.Set("challengeIssued", Napi::String::New(env, std::to_string(ticket.ChallengeIssued)));
    result.Set("bootId", Napi::Buffer<UCHAR>::Copy(env, ticket.BootId, sizeof(ticket.BootId)));
    result.Set("nonce", Napi::Buffer<UCHAR>::Copy(env, ticket.Nonce, sizeof(ticket.Nonce)));
    return result;
}

Napi::Value UnlockProtection(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    PS_PROTECT_UNLOCK_REQUEST request = {};
    DWORD bytesReturned = 0;

    if (!g_commState.isConnected || info.Length() < 2 ||
        !info[0].IsBuffer() || !info[1].IsBuffer()) {
        return Napi::Boolean::New(env, false);
    }
    Napi::Buffer<UCHAR> ticket = info[0].As<Napi::Buffer<UCHAR>>();
    Napi::Buffer<UCHAR> signature = info[1].As<Napi::Buffer<UCHAR>>();
    if (ticket.Length() != sizeof(request.Ticket) ||
        signature.Length() != PS_PROTECT_SIGNATURE_LEN) {
        return Napi::Boolean::New(env, false);
    }
    memcpy(&request.Ticket, ticket.Data(), sizeof(request.Ticket));
    request.SignatureLength = static_cast<ULONG>(signature.Length());
    memcpy(request.Signature, signature.Data(), signature.Length());

    BOOL ok = DeviceIoControl(
        g_commState.deviceHandle,
        IOCTL_PS_UNLOCK_PROTECTION,
        &request,
        sizeof(request),
        nullptr,
        0,
        &bytesReturned,
        nullptr);
    SecureZeroMemory(&request, sizeof(request));
    return Napi::Boolean::New(env, ok != FALSE);
}

Napi::Value RelockProtection(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    DWORD bytesReturned = 0;
    if (!g_commState.isConnected) return Napi::Boolean::New(env, false);
    BOOL ok = DeviceIoControl(g_commState.deviceHandle, IOCTL_PS_RELOCK_PROTECTION,
        nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    return Napi::Boolean::New(env, ok != FALSE);
}

Napi::Value SetProtectionMaintenanceMode(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!g_commState.isConnected || info.Length() < 1) return Napi::Boolean::New(env, false);
    ULONG action = info[0].ToBoolean().Value()
        ? PS_PROTECT_CONTROL_UNLOCK
        : PS_PROTECT_CONTROL_RELOCK;
    return Napi::Boolean::New(env, SendProtectControl(g_commState.deviceHandle, action));
}

Napi::Value QueryProtectionState(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    PS_PROTECT_STATUS status = {};
    DWORD bytesReturned = 0;
    if (!g_commState.isConnected) return env.Null();
    BOOL ok = DeviceIoControl(g_commState.deviceHandle, IOCTL_PS_QUERY_PROTECT_STATE,
        nullptr, 0, &status, sizeof(status), &bytesReturned, nullptr);
    if (!ok || bytesReturned != sizeof(status)) return env.Null();

    Napi::Object result = Napi::Object::New(env);
    result.Set("version", Napi::Number::New(env, status.Version));
    result.Set("enabled", Napi::Boolean::New(env, status.Enabled != 0));
    result.Set("locked", Napi::Boolean::New(env, status.Mode == 0));
    result.Set("remainingTtlSeconds", Napi::Number::New(env, status.RemainingTtlSeconds));
    result.Set("challengeActive", Napi::Boolean::New(env, status.ChallengeActive != 0));
    result.Set("pathCount", Napi::Number::New(env, status.PathCount));
    result.Set("pidCount", Napi::Number::New(env, status.PidCount));
    result.Set("failedUnlocks", Napi::Number::New(env, status.FailedUnlocks));
    result.Set("processImageCount", Napi::Number::New(env, status.ProcessImageCount));
    return result;
}

Napi::Value SetProtectionList(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    PS_PROTECT_LIST list = {};
    DWORD bytesReturned = 0;

    if (!g_commState.isConnected || info.Length() < 1 || !info[0].IsObject()) {
        return Napi::Boolean::New(env, false);
    }
    Napi::Object input = info[0].As<Napi::Object>();
    list.Enabled = !input.Has("enabled") || input.Get("enabled").ToBoolean().Value();
    if (input.Has("paths") && input.Get("paths").IsArray()) {
        Napi::Array paths = input.Get("paths").As<Napi::Array>();
        if (paths.Length() > PS_PROTECT_MAX_PATHS) return Napi::Boolean::New(env, false);
        for (uint32_t i = 0; i < paths.Length(); ++i) {
            if (!paths.Get(i).IsString()) return Napi::Boolean::New(env, false);
            std::u16string value = paths.Get(i).As<Napi::String>().Utf16Value();
            std::wstring kernelPath = ToKernelDevicePath(
                std::wstring(reinterpret_cast<const wchar_t*>(value.data()), value.size()));
            if (kernelPath.empty() || kernelPath.size() >= PS_PROTECT_PATH_LEN || kernelPath[0] != L'\\') {
                return Napi::Boolean::New(env, false);
            }
            wmemcpy(list.Paths[i], kernelPath.c_str(), kernelPath.size());
            list.Paths[i][kernelPath.size()] = L'\0';
            ++list.PathCount;
        }
    }
    if (input.Has("processIds") && input.Get("processIds").IsArray()) {
        Napi::Array pids = input.Get("processIds").As<Napi::Array>();
        if (pids.Length() > PS_PROTECT_MAX_PIDS) return Napi::Boolean::New(env, false);
        for (uint32_t i = 0; i < pids.Length(); ++i) {
            if (!pids.Get(i).IsNumber()) return Napi::Boolean::New(env, false);
            ULONG pid = pids.Get(i).As<Napi::Number>().Uint32Value();
            if (pid == 0) return Napi::Boolean::New(env, false);
            list.Pids[list.PidCount++] = pid;
        }
    }
    if (input.Has("processImages") && input.Get("processImages").IsArray()) {
        Napi::Array images = input.Get("processImages").As<Napi::Array>();
        if (images.Length() > PS_PROTECT_MAX_PROCESS_IMAGES) return Napi::Boolean::New(env, false);
        for (uint32_t i = 0; i < images.Length(); ++i) {
            if (!images.Get(i).IsString()) return Napi::Boolean::New(env, false);
            std::u16string value = images.Get(i).As<Napi::String>().Utf16Value();
            std::wstring kernelPath = ToKernelDevicePath(
                std::wstring(reinterpret_cast<const wchar_t*>(value.data()), value.size()));
            if (kernelPath.empty() || kernelPath.size() >= PS_PROTECT_PATH_LEN || kernelPath[0] != L'\\') {
                return Napi::Boolean::New(env, false);
            }
            wmemcpy(list.ProcessImages[i], kernelPath.c_str(), kernelPath.size());
            list.ProcessImages[i][kernelPath.size()] = L'\0';
            ++list.ProcessImageCount;
        }
    }

    BOOL ok = DeviceIoControl(g_commState.deviceHandle, IOCTL_PS_SET_PROTECT_LIST,
        &list, sizeof(list), nullptr, 0, &bytesReturned, nullptr);
    SecureZeroMemory(&list, sizeof(list));
    return Napi::Boolean::New(env, ok != FALSE);
}

Napi::Value WaitForEvents(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    ULONG timeoutMs = 1000;

    if (!g_commState.isConnected || g_commState.deviceHandle == INVALID_HANDLE_VALUE) {
        Napi::Object result = Napi::Object::New(env);
        Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
        result.Set("readyMask", Napi::Number::New(env, 0));
        result.Set("timedOut", Napi::Boolean::New(env, true));
        deferred.Resolve(result);
        return deferred.Promise();
    }

    if (info.Length() >= 1 && info[0].IsNumber()) {
        timeoutMs = info[0].As<Napi::Number>().Uint32Value();
    }

    auto* worker = new WaitForEventsWorker(env, timeoutMs);
    auto promise = worker->Promise();
    worker->Queue();
    return promise;
}

Napi::Value SetQuarantineConfig(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_commState.isConnected || info.Length() < 1 || !info[0].IsObject()) {
        return Napi::Boolean::New(env, false);
    }

    Napi::Object input = info[0].As<Napi::Object>();
    PS_QUARANTINE_SETTINGS settings = {};
    DWORD bytesReturned = 0;

    settings.Enabled =
        (input.Has("enabled") && input.Get("enabled").ToBoolean().Value()) ? 1 : 0;

    if (input.Has("rootDirectory") && input.Get("rootDirectory").IsString()) {
        std::u16string root = input.Get("rootDirectory").As<Napi::String>().Utf16Value();
        size_t copy = root.size();
        if (copy >= ARRAYSIZE(settings.RootDirectory)) {
            copy = ARRAYSIZE(settings.RootDirectory) - 1;
        }
        for (size_t i = 0; i < copy; i++) {
            settings.RootDirectory[i] = (WCHAR)root[i];
        }
        settings.RootDirectory[copy] = L'\0';
    }

    BOOL result = DeviceIoControl(
        g_commState.deviceHandle,
        IOCTL_PS_SET_QUARANTINE,
        &settings,
        sizeof(settings),
        NULL,
        0,
        &bytesReturned,
        NULL);

    return Napi::Boolean::New(env, result != 0);
}

Napi::Value GetQuarantineConfig(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_commState.isConnected) {
        return env.Null();
    }

    PS_QUARANTINE_SETTINGS settings = {};
    DWORD bytesReturned = 0;
    BOOL result = DeviceIoControl(
        g_commState.deviceHandle,
        IOCTL_PS_GET_QUARANTINE,
        NULL,
        0,
        &settings,
        sizeof(settings),
        &bytesReturned,
        NULL);

    if (!result || bytesReturned < sizeof(settings)) {
        return env.Null();
    }

    size_t rootLen = ClampWLen(settings.RootDirectory, ARRAYSIZE(settings.RootDirectory) - 1);
    Napi::Object output = Napi::Object::New(env);
    output.Set("enabled", Napi::Boolean::New(env, settings.Enabled != 0));
    output.Set(
        "rootDirectory",
        Napi::String::New(env, reinterpret_cast<const char16_t*>(settings.RootDirectory), rootLen));
    return output;
}

Napi::Value SetRedirectConfig(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_commState.isConnected || info.Length() < 1 || !info[0].IsObject()) {
        return Napi::Boolean::New(env, false);
    }

    Napi::Object input = info[0].As<Napi::Object>();
    PS_REDIRECT_SETTINGS settings = {};
    DWORD bytesReturned = 0;
    BOOL result;

    settings.Enabled =
        (input.Has("enabled") && input.Get("enabled").ToBoolean().Value()) ? 1 : 0;
    if (input.Has("proxyProcessId") && input.Get("proxyProcessId").IsNumber()) {
        settings.ProxyProcessId = input.Get("proxyProcessId").As<Napi::Number>().Uint32Value();
    }
    if (input.Has("proxyPort") && input.Get("proxyPort").IsNumber()) {
        settings.ProxyPort = (UINT16)input.Get("proxyPort").As<Napi::Number>().Uint32Value();
    }
    if (input.Has("addressFamily") && input.Get("addressFamily").IsNumber()) {
        settings.AddressFamily = (UINT8)input.Get("addressFamily").As<Napi::Number>().Uint32Value();
    }
    if (input.Has("proxyAddress") && input.Get("proxyAddress").IsString()) {
        std::string proxyAddress = input.Get("proxyAddress").As<Napi::String>().Utf8Value();
        if (settings.AddressFamily == AF_INET) {
            DWORD v4Address = 0;
            if (ParseIpv4Address(proxyAddress, &v4Address)) {
                memcpy(settings.ProxyAddress, &v4Address, sizeof(v4Address));
            }
        } else if (settings.AddressFamily == AF_INET6) {
            ParseIpv6Address(proxyAddress, settings.ProxyAddress);
        }
    }

    result = DeviceIoControl(
        g_commState.deviceHandle,
        IOCTL_PS_SET_REDIRECT,
        &settings,
        sizeof(settings),
        NULL,
        0,
        &bytesReturned,
        NULL);

    return Napi::Boolean::New(env, result != 0);
}

Napi::Value QueryRedirectDestination(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object output = Napi::Object::New(env);
    PS_REDIRECT_QUERY query = {};
    DWORD bytesReturned = 0;
    BOOL result;
    char addressBuffer[64] = {};

    output.Set("found", Napi::Boolean::New(env, false));

    if (!g_commState.isConnected || info.Length() < 1 || !info[0].IsObject()) {
        return output;
    }

    Napi::Object input = info[0].As<Napi::Object>();
    if (input.Has("processId") && input.Get("processId").IsNumber()) {
        query.ProcessId = input.Get("processId").As<Napi::Number>().Uint32Value();
    }
    if (input.Has("protocol") && input.Get("protocol").IsNumber()) {
        query.Protocol = (UINT8)input.Get("protocol").As<Napi::Number>().Uint32Value();
    }
    if (input.Has("addressFamily") && input.Get("addressFamily").IsNumber()) {
        query.AddressFamily = (UINT8)input.Get("addressFamily").As<Napi::Number>().Uint32Value();
    }
    if (input.Has("localPort") && input.Get("localPort").IsNumber()) {
        query.LocalPort = (UINT16)input.Get("localPort").As<Napi::Number>().Uint32Value();
    }
    if (input.Has("localAddress") && input.Get("localAddress").IsString()) {
        std::string localAddress = input.Get("localAddress").As<Napi::String>().Utf8Value();
        if (query.AddressFamily == AF_INET) {
            DWORD v4Address = 0;
            if (ParseIpv4Address(localAddress, &v4Address)) {
                memcpy(query.LocalAddress, &v4Address, sizeof(v4Address));
            }
        } else if (query.AddressFamily == AF_INET6) {
            ParseIpv6Address(localAddress, query.LocalAddress);
        }
    }

    result = DeviceIoControl(
        g_commState.deviceHandle,
        IOCTL_PS_QUERY_REDIRECT,
        &query,
        sizeof(query),
        &query,
        sizeof(query),
        &bytesReturned,
        NULL);
    if (!result || bytesReturned < sizeof(query) || query.Found == 0) {
        return output;
    }

    if (query.AddressFamily == AF_INET) {
        InetNtopA(AF_INET, query.OriginalRemoteAddress, addressBuffer, ARRAYSIZE(addressBuffer));
    } else if (query.AddressFamily == AF_INET6) {
        InetNtopA(AF_INET6, query.OriginalRemoteAddress, addressBuffer, ARRAYSIZE(addressBuffer));
    }

    output.Set("found", Napi::Boolean::New(env, true));
    output.Set("originalRemoteAddress", Napi::String::New(env, addressBuffer));
    output.Set("originalRemotePort", Napi::Number::New(env, query.OriginalRemotePort));
    return output;
}

Napi::Value LookupConnectionProcess(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object result = Napi::Object::New(env);
    std::string localAddress;
    std::string remoteAddress;
    uint32_t localPort = 0;
    uint32_t remotePort = 0;
    DWORD pid = 0;
    std::wstring processName;

    result.Set("pid", Napi::Number::New(env, 0));
    result.Set("processName", Napi::String::New(env, ""));

    if (info.Length() < 1 || !info[0].IsObject()) {
        return result;
    }

    Napi::Object input = info[0].As<Napi::Object>();
    if (input.Has("localAddress") && input.Get("localAddress").IsString()) {
        localAddress = input.Get("localAddress").As<Napi::String>().Utf8Value();
    }
    if (input.Has("remoteAddress") && input.Get("remoteAddress").IsString()) {
        remoteAddress = input.Get("remoteAddress").As<Napi::String>().Utf8Value();
    }
    if (input.Has("localPort") && input.Get("localPort").IsNumber()) {
        localPort = input.Get("localPort").As<Napi::Number>().Uint32Value();
    }
    if (input.Has("remotePort") && input.Get("remotePort").IsNumber()) {
        remotePort = input.Get("remotePort").As<Napi::Number>().Uint32Value();
    }

    if (localAddress.find(':') != std::string::npos || remoteAddress.find(':') != std::string::npos) {
        pid = FindTcpOwnerPidV6(localAddress, (USHORT)localPort, remoteAddress, (USHORT)remotePort);
    } else {
        pid = FindTcpOwnerPidV4(localAddress, (USHORT)localPort, remoteAddress, (USHORT)remotePort);
    }

    if (pid != 0) {
        processName = PsUtils::GetProcessNameById(pid);
    }

    result.Set("pid", Napi::Number::New(env, pid));
    result.Set(
        "processName",
        Napi::String::New(
            env,
            reinterpret_cast<const char16_t*>(processName.c_str()),
            processName.size()));
    return result;
}

/*++
 * ReadFileEvent —— 通过 IOCTL_PS_FILE_EVENT 取一条文件事件，解析成 JS 对象。
 * 无事件返回 { hasEvent: false }。
 * --*/
Napi::Value ReadFileEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_commState.isConnected) {
        Napi::Object o = Napi::Object::New(env);
        o.Set("hasEvent", Napi::Boolean::New(env, false));
        return o;
    }

    DLP_FILE_EVENT ev;
    DWORD bytesReturned = 0;
    BOOL result = DeviceIoControl(
        g_commState.deviceHandle, IOCTL_PS_FILE_EVENT,
        NULL, 0, &ev, sizeof(ev), &bytesReturned, NULL);

    if (!result || bytesReturned < sizeof(DLP_FILE_EVENT)) {
        Napi::Object o = Napi::Object::New(env);
        o.Set("hasEvent", Napi::Boolean::New(env, false));
        return o;
    }

    Napi::Object o = Napi::Object::New(env);
    o.Set("hasEvent", Napi::Boolean::New(env, true));
    o.Set("type", Napi::String::New(env, EventTypeToStr(ev.EventType)));
    o.Set("processId", Napi::Number::New(env, ev.ProcessId));
    // 防御：长度钳到数组容量内并截到首个 NUL，避免内核/native 结构不同步时越界读崩溃
    {
        size_t pnLen = ev.ProcessNameLength; if (pnLen > 259) pnLen = 259;   // ProcessName[260]
        pnLen = ClampWLen(ev.ProcessName, pnLen);
        size_t fnLen = ev.FileNameLength; if (fnLen > 511) fnLen = 511;      // FileName[512]
        fnLen = ClampWLen(ev.FileName, fnLen);
        size_t ofnLen = ev.OriginalFileNameLength; if (ofnLen > 511) ofnLen = 511;
        ofnLen = ClampWLen(ev.OriginalFileName, ofnLen);
        size_t qfnLen = ev.QuarantineFileNameLength; if (qfnLen > 511) qfnLen = 511;
        qfnLen = ClampWLen(ev.QuarantineFileName, qfnLen);
        o.Set("processName", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.ProcessName), pnLen));
        const std::wstring rawFileName(ev.FileName, fnLen);
        const std::wstring rawOriginalFileName(ev.OriginalFileName, ofnLen);
        const std::wstring rawQuarantineFileName(ev.QuarantineFileName, qfnLen);
        o.Set("fileName", WideStringToNapi(env, NormalizeFilePath(rawFileName)));
        o.Set("originalFileName", WideStringToNapi(env, NormalizeFilePath(rawOriginalFileName)));
        o.Set("quarantineFileName", WideStringToNapi(env, NormalizeFilePath(rawQuarantineFileName)));
        o.Set("rawFileName", WideStringToNapi(env, rawFileName));
        o.Set("rawOriginalFileName", WideStringToNapi(env, rawOriginalFileName));
        o.Set("rawQuarantineFileName", WideStringToNapi(env, rawQuarantineFileName));
    }
    o.Set("fileSize", Napi::Number::New(env, ev.FileSize));
    o.Set("action", Napi::String::New(env, ActionToStr(ev.ActionResult)));
    o.Set("timestamp", Napi::Number::New(env,
        static_cast<double>(ev.Timestamp.QuadPart)));
    o.Set("timestampMs", Napi::Number::New(env, KernelTimeToUnixMillis(ev.Timestamp)));
    return o;
}

Napi::Value ReadFileEventsBatch(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object result = Napi::Object::New(env);
    Napi::Array events = Napi::Array::New(env);
    DLP_FILE_EVENT_BATCH batch = {};
    DWORD bytesReturned = 0;
    BOOL ok;

    result.Set("hasEvent", Napi::Boolean::New(env, false));
    result.Set("events", events);
    if (!g_commState.isConnected) {
        return result;
    }

    ok = DeviceIoControl(
        g_commState.deviceHandle, IOCTL_PS_FILE_EVENT_BATCH,
        NULL, 0, &batch, sizeof(batch), &bytesReturned, NULL);
    if (!ok || bytesReturned < sizeof(ULONG)) {
        return result;
    }

    for (ULONG i = 0; i < batch.Count && i < PS_FILE_BATCH_MAX; i++) {
        const DLP_FILE_EVENT& ev = batch.Events[i];
        Napi::Object o = Napi::Object::New(env);
        size_t pnLen = ev.ProcessNameLength; if (pnLen > 259) pnLen = 259;
        pnLen = ClampWLen(ev.ProcessName, pnLen);
        size_t fnLen = ev.FileNameLength; if (fnLen > 511) fnLen = 511;
        fnLen = ClampWLen(ev.FileName, fnLen);
        size_t ofnLen = ev.OriginalFileNameLength; if (ofnLen > 511) ofnLen = 511;
        ofnLen = ClampWLen(ev.OriginalFileName, ofnLen);
        size_t qfnLen = ev.QuarantineFileNameLength; if (qfnLen > 511) qfnLen = 511;
        qfnLen = ClampWLen(ev.QuarantineFileName, qfnLen);
        o.Set("hasEvent", Napi::Boolean::New(env, true));
        o.Set("type", Napi::String::New(env, EventTypeToStr(ev.EventType)));
        o.Set("processId", Napi::Number::New(env, ev.ProcessId));
        o.Set("processName", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.ProcessName), pnLen));
        const std::wstring rawFileName(ev.FileName, fnLen);
        const std::wstring rawOriginalFileName(ev.OriginalFileName, ofnLen);
        const std::wstring rawQuarantineFileName(ev.QuarantineFileName, qfnLen);
        o.Set("fileName", WideStringToNapi(env, NormalizeFilePath(rawFileName)));
        o.Set("originalFileName", WideStringToNapi(env, NormalizeFilePath(rawOriginalFileName)));
        o.Set("quarantineFileName", WideStringToNapi(env, NormalizeFilePath(rawQuarantineFileName)));
        o.Set("rawFileName", WideStringToNapi(env, rawFileName));
        o.Set("rawOriginalFileName", WideStringToNapi(env, rawOriginalFileName));
        o.Set("rawQuarantineFileName", WideStringToNapi(env, rawQuarantineFileName));
        o.Set("fileSize", Napi::Number::New(env, ev.FileSize));
        o.Set("action", Napi::String::New(env, ActionToStr(ev.ActionResult)));
        o.Set("timestamp", Napi::Number::New(env, static_cast<double>(ev.Timestamp.QuadPart)));
        o.Set("timestampMs", Napi::Number::New(env, KernelTimeToUnixMillis(ev.Timestamp)));
        events.Set(i, o);
    }

    result.Set("hasEvent", Napi::Boolean::New(env, batch.Count > 0));
    result.Set("count", Napi::Number::New(env, batch.Count));
    return result;
}

/*++
 * ReadNetEvent —— 通过 IOCTL_PS_NET_EVENT 取一条网络事件，解析成 JS 对象。
 * --*/
Napi::Value ReadNetEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_commState.isConnected) {
        Napi::Object o = Napi::Object::New(env);
        o.Set("hasEvent", Napi::Boolean::New(env, false));
        return o;
    }

    DLP_NET_EVENT ev;
    DWORD bytesReturned = 0;
    BOOL result = DeviceIoControl(
        g_commState.deviceHandle, IOCTL_PS_NET_EVENT,
        NULL, 0, &ev, sizeof(ev), &bytesReturned, NULL);

    if (!result || bytesReturned < sizeof(DLP_NET_EVENT)) {
        Napi::Object o = Napi::Object::New(env);
        o.Set("hasEvent", Napi::Boolean::New(env, false));
        return o;
    }

    Napi::Object o = Napi::Object::New(env);
    o.Set("hasEvent", Napi::Boolean::New(env, true));
    o.Set("type", Napi::String::New(env, EventTypeToStr(ev.EventType)));
    o.Set("processId", Napi::Number::New(env, ev.ProcessId));
    // 防御：全部长度钳到容量内并截到首个 NUL
    {
        size_t pnLen = ev.ProcessNameLength; if (pnLen > 255) pnLen = 255;   // ProcessName[256]
        pnLen = ClampWLen(ev.ProcessName, pnLen);
        size_t raLen = ev.RemoteAddressLength; if (raLen > 127) raLen = 127; // RemoteAddress[128]
        raLen = ClampALen(reinterpret_cast<const char*>(ev.RemoteAddress), raLen);
        size_t laLen = ev.LocalAddressLength; if (laLen > 127) laLen = 127;  // LocalAddress[128]
        laLen = ClampALen(reinterpret_cast<const char*>(ev.LocalAddress), laLen);
        size_t urlLen = ev.UrlLength; if (urlLen > 1023) urlLen = 1023;      // Url[1024]
        urlLen = ClampWLen(ev.Url, urlLen);
        size_t sniLen = ev.SniDomainLength; if (sniLen > 255) sniLen = 255;  // SniDomain[256]
        sniLen = ClampWLen(ev.SniDomain, sniLen);
        size_t ctLen = ev.ContentTypeLength; if (ctLen > 63) ctLen = 63;     // ContentType[64]
        ctLen = ClampWLen(ev.ContentType, ctLen);
        size_t ceLen = ev.ContentEncodingLength; if (ceLen > 31) ceLen = 31; // ContentEncoding[32]
        ceLen = ClampWLen(ev.ContentEncoding, ceLen);
        size_t bpLen = ev.BodyPreviewLength; if (bpLen > 2048) bpLen = 2048; // BodyPreview[2048]
        o.Set("processName", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.ProcessName), pnLen));
        o.Set("remoteAddress", Napi::String::New(env,
            reinterpret_cast<const char*>(ev.RemoteAddress), raLen));
        o.Set("localAddress", Napi::String::New(env,
            reinterpret_cast<const char*>(ev.LocalAddress), laLen));
        o.Set("url", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.Url), urlLen));
        o.Set("sniDomain", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.SniDomain), sniLen));
        o.Set("contentType", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.ContentType), ctLen));
        o.Set("contentEncoding", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.ContentEncoding), ceLen));
        o.Set("bodyPreview", Napi::Buffer<UCHAR>::Copy(env, ev.BodyPreview, bpLen));
    }
    o.Set("remotePort", Napi::Number::New(env, ev.RemotePort));
    o.Set("localPort", Napi::Number::New(env, ev.LocalPort));
    o.Set("protocol", Napi::Number::New(env, ev.Protocol));
    o.Set("action", Napi::String::New(env, ActionToStr(ev.ActionResult)));
    o.Set("timestamp", Napi::Number::New(env,
        static_cast<double>(ev.Timestamp.QuadPart)));
    o.Set("timestampMs", Napi::Number::New(env, KernelTimeToUnixMillis(ev.Timestamp)));
    return o;
}

Napi::Value ReadNetEventsBatch(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object result = Napi::Object::New(env);
    Napi::Array events = Napi::Array::New(env);
    DLP_NET_EVENT_BATCH batch = {};
    DWORD bytesReturned = 0;
    BOOL ok;

    result.Set("hasEvent", Napi::Boolean::New(env, false));
    result.Set("events", events);
    if (!g_commState.isConnected) {
        return result;
    }

    ok = DeviceIoControl(
        g_commState.deviceHandle, IOCTL_PS_NET_EVENT_BATCH,
        NULL, 0, &batch, sizeof(batch), &bytesReturned, NULL);
    if (!ok || bytesReturned < sizeof(ULONG)) {
        return result;
    }

    for (ULONG i = 0; i < batch.Count && i < PS_NET_BATCH_MAX; i++) {
        const DLP_NET_EVENT& ev = batch.Events[i];
        Napi::Object o = Napi::Object::New(env);
        size_t pnLen = ev.ProcessNameLength; if (pnLen > 255) pnLen = 255;
        pnLen = ClampWLen(ev.ProcessName, pnLen);
        size_t raLen = ev.RemoteAddressLength; if (raLen > 127) raLen = 127;
        raLen = ClampALen(reinterpret_cast<const char*>(ev.RemoteAddress), raLen);
        size_t laLen = ev.LocalAddressLength; if (laLen > 127) laLen = 127;
        laLen = ClampALen(reinterpret_cast<const char*>(ev.LocalAddress), laLen);
        size_t urlLen = ev.UrlLength; if (urlLen > 1023) urlLen = 1023;
        urlLen = ClampWLen(ev.Url, urlLen);
        size_t sniLen = ev.SniDomainLength; if (sniLen > 255) sniLen = 255;
        sniLen = ClampWLen(ev.SniDomain, sniLen);
        size_t ctLen = ev.ContentTypeLength; if (ctLen > 63) ctLen = 63;
        ctLen = ClampWLen(ev.ContentType, ctLen);
        size_t ceLen = ev.ContentEncodingLength; if (ceLen > 31) ceLen = 31;
        ceLen = ClampWLen(ev.ContentEncoding, ceLen);
        size_t bpLen = ev.BodyPreviewLength; if (bpLen > 2048) bpLen = 2048;
        o.Set("hasEvent", Napi::Boolean::New(env, true));
        o.Set("type", Napi::String::New(env, EventTypeToStr(ev.EventType)));
        o.Set("processId", Napi::Number::New(env, ev.ProcessId));
        o.Set("processName", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.ProcessName), pnLen));
        o.Set("remoteAddress", Napi::String::New(env,
            reinterpret_cast<const char*>(ev.RemoteAddress), raLen));
        o.Set("localAddress", Napi::String::New(env,
            reinterpret_cast<const char*>(ev.LocalAddress), laLen));
        o.Set("url", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.Url), urlLen));
        o.Set("sniDomain", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.SniDomain), sniLen));
        o.Set("contentType", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.ContentType), ctLen));
        o.Set("contentEncoding", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ev.ContentEncoding), ceLen));
        o.Set("bodyPreview", Napi::Buffer<UCHAR>::Copy(env, ev.BodyPreview, bpLen));
        o.Set("remotePort", Napi::Number::New(env, ev.RemotePort));
        o.Set("localPort", Napi::Number::New(env, ev.LocalPort));
        o.Set("protocol", Napi::Number::New(env, ev.Protocol));
        o.Set("action", Napi::String::New(env, ActionToStr(ev.ActionResult)));
        o.Set("timestamp", Napi::Number::New(env, static_cast<double>(ev.Timestamp.QuadPart)));
        o.Set("timestampMs", Napi::Number::New(env, KernelTimeToUnixMillis(ev.Timestamp)));
        events.Set(i, o);
    }

    result.Set("hasEvent", Napi::Boolean::New(env, batch.Count > 0));
    result.Set("count", Napi::Number::New(env, batch.Count));
    return result;
}

/*++
 * InitKernelCommAddon
 * --*/
Napi::Object InitKernelCommAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("connect", Napi::Function::New(env, ConnectToDevice));
    exports.Set("disconnect", Napi::Function::New(env, DisconnectFromDevice));
    exports.Set("getConnectionState", Napi::Function::New(env, GetConnectionState));
    exports.Set("sendIoctl", Napi::Function::New(env, SendIoctl));
    exports.Set("getDriverStatus", Napi::Function::New(env, GetDriverStatus));
    exports.Set("getProtectionChallenge", Napi::Function::New(env, GetProtectionChallenge));
    exports.Set("unlockProtection", Napi::Function::New(env, UnlockProtection));
    exports.Set("relockProtection", Napi::Function::New(env, RelockProtection));
    exports.Set("getProtectionState", Napi::Function::New(env, QueryProtectionState));
    exports.Set("setProtectionList", Napi::Function::New(env, SetProtectionList));
    exports.Set("setProtectionMaintenanceMode", Napi::Function::New(env, SetProtectionMaintenanceMode));
    exports.Set("waitForEvents", Napi::Function::New(env, WaitForEvents));
    exports.Set("setQuarantineConfig", Napi::Function::New(env, SetQuarantineConfig));
    exports.Set("getQuarantineConfig", Napi::Function::New(env, GetQuarantineConfig));
    exports.Set("setRedirectConfig", Napi::Function::New(env, SetRedirectConfig));
    exports.Set("queryRedirectDestination", Napi::Function::New(env, QueryRedirectDestination));
    exports.Set("lookupConnectionProcess", Napi::Function::New(env, LookupConnectionProcess));
    exports.Set("normalizeFilePath", Napi::Function::New(env, NormalizeFilePathForJs));
    exports.Set("readFileEvent", Napi::Function::New(env, ReadFileEvent));
    exports.Set("readFileEventsBatch", Napi::Function::New(env, ReadFileEventsBatch));
    exports.Set("readNetEvent", Napi::Function::New(env, ReadNetEvent));
    exports.Set("readNetEventsBatch", Napi::Function::New(env, ReadNetEventsBatch));
    return exports;
}
