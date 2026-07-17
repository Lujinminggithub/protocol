/*
 * driver_loader.cpp - MiniFilter 驱动加载/卸载实现
 *
 * 通过 SCM 创建“文件系统驱动(filesys)”服务、写入 MiniFilter 必需的
 * Instances/Altitude 注册表，然后用 FilterLoad 加载。
 * .sys 源路径由 JS 侧传入（打包后位于 resources/driver/）。
 */

#include "driver_loader.h"
#include <windows.h>
#include <winsvc.h>
#include <fltuser.h>
#include <string>

#pragma comment(lib, "fltlib.lib")
#pragma comment(lib, "advapi32.lib")

static const wchar_t* kServiceName = L"PersonalSafer";
static const wchar_t* kDisplayName = L"PersonalSafer Security Driver";
static const wchar_t* kAltitude    = L"379950";                 // 必须与 driver.h 的 PS_ALTITUDE 一致
static const wchar_t* kInstanceName = L"PersonalSafer Instance";

// ---- 工具: 管理员检查 ----
static bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

// ---- 工具: 启用某项特权(如 SeLoadDriverPrivilege) ----
static bool EnablePrivilege(LPCWSTR privName) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }
    LUID luid;
    bool ok = false;
    if (LookupPrivilegeValueW(NULL, privName, &luid)) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
        ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(hToken);
    return ok;
}

static bool SetupMiniFilterRegistryPath(const wchar_t* instancesPath) {
    HKEY hInstances = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, instancesPath, 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hInstances, NULL)
            != ERROR_SUCCESS) {
        return false;
    }

    LSTATUS result = RegSetValueExW(hInstances, L"DefaultInstance", 0, REG_SZ,
        (const BYTE*)kInstanceName,
        (DWORD)((wcslen(kInstanceName) + 1) * sizeof(wchar_t)));
    RegCloseKey(hInstances);
    if (result != ERROR_SUCCESS) {
        return false;
    }

    HKEY hInst = NULL;
    std::wstring instPath = std::wstring(instancesPath) + L"\\" + kInstanceName;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, instPath.c_str(), 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hInst, NULL)
            != ERROR_SUCCESS) {
        return false;
    }

    result = RegSetValueExW(hInst, L"Altitude", 0, REG_SZ,
        (const BYTE*)kAltitude,
        (DWORD)((wcslen(kAltitude) + 1) * sizeof(wchar_t)));
    DWORD flags = 0;
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(hInst, L"Flags", 0, REG_DWORD,
            (const BYTE*)&flags, sizeof(flags));
    }
    RegCloseKey(hInst);
    return result == ERROR_SUCCESS;
}

// Write both layouts for Windows 10 and Windows 11 24H2 compatibility.
static bool SetupMiniFilterRegistry() {
    const wchar_t* legacyPath =
        L"SYSTEM\\CurrentControlSet\\Services\\PersonalSafer\\Instances";
    const wchar_t* parametersPath =
        L"SYSTEM\\CurrentControlSet\\Services\\PersonalSafer\\Parameters\\Instances";
    return SetupMiniFilterRegistryPath(legacyPath) &&
        SetupMiniFilterRegistryPath(parametersPath);
}

static Napi::Object MakeResult(Napi::Env env, bool success, const std::string& err = "") {
    Napi::Object r = Napi::Object::New(env);
    r.Set("success", Napi::Boolean::New(env, success));
    if (!err.empty()) r.Set("error", Napi::String::New(env, err));
    return r;
}

static bool IsMiniFilterLoaded() {
    HANDLE hFilterFind = NULL;
    BYTE buffer[1024];
    ULONG bytesReturned = 0;
    bool loaded = false;

    HRESULT hr = FilterFindFirst(FilterFullInformation, buffer, sizeof(buffer),
        &bytesReturned, &hFilterFind);
    while (SUCCEEDED(hr)) {
        PFILTER_FULL_INFORMATION fi = reinterpret_cast<PFILTER_FULL_INFORMATION>(buffer);
        std::wstring name(fi->FilterNameBuffer, fi->FilterNameLength / sizeof(WCHAR));
        if (name == kServiceName) {
            loaded = true;
            break;
        }
        hr = FilterFindNext(hFilterFind, FilterFullInformation, buffer,
            sizeof(buffer), &bytesReturned);
    }
    if (hFilterFind != NULL) {
        FilterFindClose(hFilterFind);
    }
    return loaded;
}

/*++
 * LoadDriver(sysPath?)
 *
 * 加载 MiniFilter 驱动。sysPath 为 .sys 源文件绝对路径（可选，
 * JS 侧传入打包后的 resources/driver/PersonalSafer.sys）。
 * --*/
Napi::Value LoadDriver(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!IsRunningAsAdmin()) {
        return MakeResult(env, false, "需要管理员权限加载驱动");
    }

    if (IsMiniFilterLoaded()) {
        Napi::Object r = MakeResult(env, true);
        r.Set("alreadyRunning", Napi::Boolean::New(env, true));
        return r;
    }

    // 目标: %SystemRoot%\System32\drivers\PersonalSafer.sys
    wchar_t sysDir[MAX_PATH] = {0};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring destSys = std::wstring(sysDir) + L"\\drivers\\PersonalSafer.sys";

    // 若 JS 传入了源路径，则复制到系统目录
    if (info.Length() >= 1 && info[0].IsString()) {
        std::u16string s = info[0].As<Napi::String>().Utf16Value();
        std::wstring srcSys(s.begin(), s.end());
        if (!srcSys.empty()) {
            if (!CopyFileW(srcSys.c_str(), destSys.c_str(), FALSE)) {
                DWORD e = GetLastError();
                return MakeResult(env, false,
                    "复制驱动文件失败 (错误码: " + std::to_string(e) + ")");
            }
        }
    }

    EnablePrivilege(L"SeLoadDriverPrivilege");

    // 创建/打开“文件系统驱动”服务
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (hSCM == NULL) {
        return MakeResult(env, false, "无法打开 SCM (错误码: " +
            std::to_string(GetLastError()) + ")");
    }

    SC_HANDLE hService = OpenServiceW(hSCM, kServiceName, SERVICE_ALL_ACCESS);
    bool existingService = hService != NULL;
    if (hService == NULL) {
        hService = CreateServiceW(
            hSCM, kServiceName, kDisplayName,
            SERVICE_ALL_ACCESS,
            SERVICE_FILE_SYSTEM_DRIVER,   // MiniFilter 必须为文件系统驱动
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            destSys.c_str(),
            L"FSFilter Activity Monitor", NULL, L"FltMgr\0", NULL, NULL
        );
        if (hService == NULL) {
            DWORD e = GetLastError();
            CloseServiceHandle(hSCM);
            return MakeResult(env, false,
                "无法创建驱动服务 (错误码: " + std::to_string(e) + ")");
        }
    }

    if (existingService) {
        SERVICE_STATUS_PROCESS serviceStatus = {};
        DWORD bytesNeeded = 0;
        if (!QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&serviceStatus), sizeof(serviceStatus), &bytesNeeded)) {
            DWORD e = GetLastError();
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCM);
            return MakeResult(env, false,
                "无法查询驱动服务状态 (错误码: " + std::to_string(e) + ")");
        }

        if (serviceStatus.dwCurrentState == SERVICE_STOPPED &&
            !ChangeServiceConfigW(hService,
                SERVICE_FILE_SYSTEM_DRIVER,
                SERVICE_DEMAND_START,
                SERVICE_ERROR_NORMAL,
                destSys.c_str(),
                L"FSFilter Activity Monitor", NULL, L"FltMgr\0", NULL, NULL,
                kDisplayName)) {
            DWORD e = GetLastError();
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCM);
            return MakeResult(env, false,
                "无法更新驱动服务配置 (错误码: " + std::to_string(e) + ")");
        }
    }
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);

    // MiniFilter 必需的 Instances/Altitude 注册表
    if (!SetupMiniFilterRegistry()) {
        return MakeResult(env, false, "写入 MiniFilter 注册表失败");
    }

    // 用 FilterLoad 加载 MiniFilter（等价于 fltmc load）
    HRESULT hr = FilterLoad(kServiceName);
    if (hr == S_OK || IsMiniFilterLoaded()) {
        Napi::Object r = MakeResult(env, true);
        r.Set("alreadyRunning", Napi::Boolean::New(env,
            hr != S_OK));
        return r;
    }

    char buf[32];
    sprintf_s(buf, "0x%08lX", (unsigned long)hr);
    return MakeResult(env, false, std::string("FilterLoad 失败 (HRESULT: ") + buf +
        ")，常见: 0x801F0011=实例配置, 0x800705B4=超时/驱动未响应, 0xC000037A/签名相关=未禁用驱动签名");
}

/*++
 * UnloadDriver - 卸载 MiniFilter 并删除服务
 * --*/
Napi::Value UnloadDriver(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!IsRunningAsAdmin()) {
        return MakeResult(env, false, "需要管理员权限卸载驱动");
    }

    // 卸载 MiniFilter（忽略“未加载”错误）
    FilterUnload(kServiceName);

    // 删除服务
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (hSCM != NULL) {
        SC_HANDLE hService = OpenServiceW(hSCM, kServiceName, DELETE);
        if (hService != NULL) {
            DeleteService(hService);
            CloseServiceHandle(hService);
        }
        CloseServiceHandle(hSCM);
    }

    return MakeResult(env, true);
}

/*++
 * IsDriverLoaded - 通过过滤器管理器查询是否已加载
 * --*/
Napi::Value IsDriverLoaded(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    return Napi::Boolean::New(env, IsMiniFilterLoaded());
}

/*++
 * InitDriverLoaderAddon
 * --*/
Napi::Object InitDriverLoaderAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("load", Napi::Function::New(env, LoadDriver));
    exports.Set("unload", Napi::Function::New(env, UnloadDriver));
    exports.Set("isLoaded", Napi::Function::New(env, IsDriverLoaded));
    return exports;
}
