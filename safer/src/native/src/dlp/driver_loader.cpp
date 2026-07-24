/* PersonalSafer MiniFilter service management and load-state detection. */

#include "driver_loader.h"

#include <windows.h>
#include <winsvc.h>
#include <fltuser.h>
#include <string>
#include <vector>

#pragma comment(lib, "fltlib.lib")
#pragma comment(lib, "advapi32.lib")

static const wchar_t* kServiceName = L"PersonalSafer";
static const wchar_t* kDisplayName = L"PersonalSafer Security Driver";
static const wchar_t* kAltitude = L"379950";
static const wchar_t* kInstanceName = L"PersonalSafer Instance";
static const wchar_t* kDevicePath = L"\\\\.\\PersonalSafer";

struct DriverLoadState {
    bool filterManagerLoaded;
    bool serviceRunning;
    bool deviceReachable;

    bool Loaded() const {
        return filterManagerLoaded || serviceRunning || deviceReachable;
    }
};

static bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

static bool EnablePrivilege(LPCWSTR privilegeName) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &token)) {
        return false;
    }

    LUID luid = {};
    bool enabled = false;
    if (LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        TOKEN_PRIVILEGES privileges = {};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
        enabled = GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return enabled;
}

static bool SetRegistryString(HKEY key, const wchar_t* name, const wchar_t* value) {
    return RegSetValueExW(
        key,
        name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static bool SetupMiniFilterRegistryPath(const wchar_t* instancesPath) {
    HKEY instancesKey = nullptr;
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            instancesPath,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_ALL_ACCESS,
            nullptr,
            &instancesKey,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    bool success = SetRegistryString(instancesKey, L"DefaultInstance", kInstanceName);
    RegCloseKey(instancesKey);
    if (!success) return false;

    const std::wstring instancePath =
        std::wstring(instancesPath) + L"\\" + kInstanceName;
    HKEY instanceKey = nullptr;
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            instancePath.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_ALL_ACCESS,
            nullptr,
            &instanceKey,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    DWORD flags = 0;
    success = SetRegistryString(instanceKey, L"Altitude", kAltitude) &&
        RegSetValueExW(
            instanceKey,
            L"Flags",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&flags),
            sizeof(flags)) == ERROR_SUCCESS;
    RegCloseKey(instanceKey);
    return success;
}

static bool SetupMiniFilterRegistry() {
    static const wchar_t* kServiceParameters =
        L"SYSTEM\\CurrentControlSet\\Services\\PersonalSafer\\Parameters";
    static const wchar_t* kLegacyInstances =
        L"SYSTEM\\CurrentControlSet\\Services\\PersonalSafer\\Instances";
    static const wchar_t* kParameterInstances =
        L"SYSTEM\\CurrentControlSet\\Services\\PersonalSafer\\Parameters\\Instances";

    HKEY parametersKey = nullptr;
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            kServiceParameters,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_ALL_ACCESS,
            nullptr,
            &parametersKey,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }
    DWORD supportedFeatures = 3;
    const bool parametersWritten = RegSetValueExW(
        parametersKey,
        L"SupportedFeatures",
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&supportedFeatures),
        sizeof(supportedFeatures)) == ERROR_SUCCESS;
    RegCloseKey(parametersKey);

    return parametersWritten &&
        SetupMiniFilterRegistryPath(kLegacyInstances) &&
        SetupMiniFilterRegistryPath(kParameterInstances);
}

static bool IsFilterManagerLoaded() {
    HANDLE findHandle = nullptr;
    std::vector<BYTE> buffer(64 * 1024);
    ULONG bytesReturned = 0;
    HRESULT result = FilterFindFirst(
        FilterFullInformation,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytesReturned,
        &findHandle);

    while (SUCCEEDED(result)) {
        const auto* info = reinterpret_cast<const FILTER_FULL_INFORMATION*>(buffer.data());
        const std::wstring filterName(
            info->FilterNameBuffer,
            info->FilterNameLength / sizeof(wchar_t));
        if (_wcsicmp(filterName.c_str(), kServiceName) == 0) {
            FilterFindClose(findHandle);
            return true;
        }

        result = FilterFindNext(
            findHandle,
            FilterFullInformation,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesReturned);
    }

    if (findHandle != nullptr) FilterFindClose(findHandle);
    return false;
}

static bool IsServiceRunning() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) return false;

    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD bytesNeeded = 0;
    const bool queried = QueryServiceStatusEx(
        service,
        SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status),
        sizeof(status),
        &bytesNeeded) != FALSE;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    return queried &&
        (status.dwCurrentState == SERVICE_RUNNING ||
         status.dwCurrentState == SERVICE_START_PENDING);
}

static bool IsDeviceReachable() {
    HANDLE device = CreateFileW(
        kDevicePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (device == INVALID_HANDLE_VALUE) return false;
    CloseHandle(device);
    return true;
}

static DriverLoadState QueryDriverLoadState() {
    DriverLoadState state = {};
    state.filterManagerLoaded = IsFilterManagerLoaded();
    state.serviceRunning = IsServiceRunning();
    state.deviceReachable = IsDeviceReachable();
    return state;
}

static Napi::Object MakeResult(
    Napi::Env env,
    bool success,
    const std::string& error = std::string()) {
    Napi::Object result = Napi::Object::New(env);
    result.Set("success", Napi::Boolean::New(env, success));
    if (!error.empty()) result.Set("error", Napi::String::New(env, error));
    return result;
}

Napi::Value LoadDriver(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!IsRunningAsAdmin()) {
        return MakeResult(env, false, "Administrator privileges are required to load the driver");
    }

    DriverLoadState initialState = QueryDriverLoadState();
    if (initialState.filterManagerLoaded || initialState.deviceReachable) {
        Napi::Object result = MakeResult(env, true);
        result.Set("alreadyRunning", Napi::Boolean::New(env, true));
        return result;
    }

    wchar_t systemDirectory[MAX_PATH] = {};
    if (GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0) {
        return MakeResult(env, false, "Unable to resolve the Windows system directory");
    }
    const std::wstring destination =
        std::wstring(systemDirectory) + L"\\drivers\\PersonalSafer.sys";

    if (info.Length() >= 1 && info[0].IsString()) {
        const std::u16string sourceUtf16 = info[0].As<Napi::String>().Utf16Value();
        const std::wstring source(sourceUtf16.begin(), sourceUtf16.end());
        if (!source.empty() && _wcsicmp(source.c_str(), destination.c_str()) != 0 &&
            !CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
            return MakeResult(
                env,
                false,
                "Unable to copy the driver image (Win32 error " +
                    std::to_string(GetLastError()) + ")");
        }
    }

    EnablePrivilege(L"SeLoadDriverPrivilege");
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (scm == nullptr) {
        return MakeResult(
            env,
            false,
            "Unable to open SCM (Win32 error " + std::to_string(GetLastError()) + ")");
    }

    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
    if (service == nullptr) {
        static const wchar_t dependencies[] = L"FltMgr\0\0";
        service = CreateServiceW(
            scm,
            kServiceName,
            kDisplayName,
            SERVICE_ALL_ACCESS,
            SERVICE_FILE_SYSTEM_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            destination.c_str(),
            L"FSFilter Activity Monitor",
            nullptr,
            dependencies,
            nullptr,
            nullptr);
        if (service == nullptr) {
            const DWORD error = GetLastError();
            CloseServiceHandle(scm);
            return MakeResult(
                env,
                false,
                "Unable to create the driver service (Win32 error " +
                    std::to_string(error) + ")");
        }
    } else {
        SERVICE_STATUS_PROCESS status = {};
        DWORD bytesNeeded = 0;
        if (!QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytesNeeded)) {
            const DWORD error = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return MakeResult(
                env,
                false,
                "Unable to query the driver service (Win32 error " +
                    std::to_string(error) + ")");
        }

        if (status.dwCurrentState == SERVICE_STOPPED) {
            static const wchar_t dependencies[] = L"FltMgr\0\0";
            if (!ChangeServiceConfigW(
                    service,
                    SERVICE_FILE_SYSTEM_DRIVER,
                    SERVICE_DEMAND_START,
                    SERVICE_ERROR_NORMAL,
                    destination.c_str(),
                    L"FSFilter Activity Monitor",
                    nullptr,
                    dependencies,
                    nullptr,
                    nullptr,
                    kDisplayName)) {
                const DWORD error = GetLastError();
                CloseServiceHandle(service);
                CloseServiceHandle(scm);
                return MakeResult(
                    env,
                    false,
                    "Unable to update the driver service (Win32 error " +
                        std::to_string(error) + ")");
            }
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (!SetupMiniFilterRegistry()) {
        return MakeResult(env, false, "Unable to write the MiniFilter registry configuration");
    }

    const HRESULT loadResult = FilterLoad(kServiceName);
    const DriverLoadState finalState = QueryDriverLoadState();
    if (loadResult == S_OK || finalState.Loaded()) {
        Napi::Object result = MakeResult(env, true);
        result.Set("alreadyRunning", Napi::Boolean::New(env, loadResult != S_OK));
        return result;
    }

    char resultBuffer[32] = {};
    sprintf_s(resultBuffer, "0x%08lX", static_cast<unsigned long>(loadResult));
    return MakeResult(
        env,
        false,
        std::string("FilterLoad failed (HRESULT ") + resultBuffer + ")");
}

Napi::Value UnloadDriver(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!IsRunningAsAdmin()) {
        return MakeResult(env, false, "Administrator privileges are required to unload the driver");
    }

    const HRESULT unloadResult = FilterUnload(kServiceName);
    const DriverLoadState finalState = QueryDriverLoadState();
    if (!finalState.Loaded()) {
        return MakeResult(env, true);
    }

    char resultBuffer[32] = {};
    sprintf_s(resultBuffer, "0x%08lX", static_cast<unsigned long>(unloadResult));
    return MakeResult(
        env,
        false,
        std::string("FilterUnload failed (HRESULT ") + resultBuffer + ")");
}

Napi::Value IsDriverLoaded(const Napi::CallbackInfo& info) {
    return Napi::Boolean::New(info.Env(), QueryDriverLoadState().Loaded());
}

static Napi::Value GetDriverLoadState(const Napi::CallbackInfo& info) {
    const DriverLoadState state = QueryDriverLoadState();
    Napi::Object result = Napi::Object::New(info.Env());
    result.Set("loaded", Napi::Boolean::New(info.Env(), state.Loaded()));
    result.Set(
        "filterManagerLoaded",
        Napi::Boolean::New(info.Env(), state.filterManagerLoaded));
    result.Set("serviceRunning", Napi::Boolean::New(info.Env(), state.serviceRunning));
    result.Set("deviceReachable", Napi::Boolean::New(info.Env(), state.deviceReachable));
    return result;
}

Napi::Object InitDriverLoaderAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("load", Napi::Function::New(env, LoadDriver));
    exports.Set("unload", Napi::Function::New(env, UnloadDriver));
    exports.Set("isLoaded", Napi::Function::New(env, IsDriverLoaded));
    exports.Set("getLoadState", Napi::Function::New(env, GetDriverLoadState));
    return exports;
}
