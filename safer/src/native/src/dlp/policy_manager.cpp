/*
 * policy_manager.cpp - policy IOCTL bridge
 */

#include "policy_manager.h"
#include "ps_shared.h"
#include <windows.h>
#include <cwchar>
#include <string>

static const wchar_t* kDevicePath = L"\\\\.\\PersonalSafer";

static ULONG
FillWStringArray(
    const Napi::Array& arr,
    WCHAR* base,
    ULONG maxRows,
    ULONG cellLen
    )
{
    ULONG n = 0;

    for (uint32_t i = 0; i < arr.Length() && n < maxRows; i++) {
        Napi::Value v = arr.Get(i);
        if (!v.IsString()) {
            continue;
        }

        std::u16string s = v.As<Napi::String>().Utf16Value();
        WCHAR* row = base + (SIZE_T)n * cellLen;
        SIZE_T copy = s.size() < (SIZE_T)(cellLen - 1) ? s.size() : (SIZE_T)(cellLen - 1);

        for (SIZE_T k = 0; k < copy; k++) {
            row[k] = (WCHAR)towlower((wint_t)s[k]);
        }
        row[copy] = L'\0';
        n++;
    }

    return n;
}

static void
AppendWStringArray(
    Napi::Env env,
    Napi::Array& target,
    const WCHAR* base,
    ULONG count,
    ULONG maxRows,
    ULONG cellLen
    )
{
    ULONG safeCount = count < maxRows ? count : maxRows;

    for (ULONG i = 0; i < safeCount; i++) {
        const WCHAR* row = base + (SIZE_T)i * cellLen;
        size_t rowLen = wcsnlen(row, cellLen);
        if (rowLen == 0) {
            continue;
        }

        target.Set(
            target.Length(),
            Napi::String::New(env, reinterpret_cast<const char16_t*>(row), rowLen));
    }
}

static HANDLE
OpenPolicyDevice()
{
    return CreateFileW(
        kDevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
}

Napi::Value
GetPolicy(const Napi::CallbackInfo& info)
{
    Napi::Env env = info.Env();
    POLICY_COMMAND cmd;
    DWORD bytesReturned = 0;
    HANDLE h;
    BOOL ok;
    Napi::Object policy;
    Napi::Array blockedExtensions;
    Napi::Array processBlacklist;
    Napi::Array blockedPorts;
    Napi::Array blockedDomains;
    Napi::Array blockedUrls;
    Napi::Array blockedFtpCommands;
    Napi::Array blockedFtpPaths;
    Napi::Array blockedFtpContentPatterns;
    Napi::Array blockedHttpHeaders;
    Napi::Array blockedHttpTrailers;
    Napi::Array blockedHttpBodyPatterns;
    Napi::Array blockedJsonKeys;
    Napi::Array blockedJsonPaths;
    Napi::Array blockedJsonValues;

    ZeroMemory(&cmd, sizeof(cmd));
    cmd.CommandId = CMD_GET_POLICY;

    h = OpenPolicyDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return env.Null();
    }

    ok = DeviceIoControl(
        h,
        IOCTL_PS_GET_POLICY,
        NULL,
        0,
        &cmd,
        sizeof(cmd),
        &bytesReturned,
        NULL);
    CloseHandle(h);

    if (!ok || bytesReturned < sizeof(POLICY_COMMAND) || cmd.PolicySize < sizeof(PS_POLICY_DATA)) {
        return env.Null();
    }

    const PS_POLICY_DATA* pol = reinterpret_cast<const PS_POLICY_DATA*>(cmd.PolicyData);
    policy = Napi::Object::New(env);
    blockedExtensions = Napi::Array::New(env);
    processBlacklist = Napi::Array::New(env);
    blockedPorts = Napi::Array::New(env);
    blockedDomains = Napi::Array::New(env);
    blockedUrls = Napi::Array::New(env);
    blockedFtpCommands = Napi::Array::New(env);
    blockedFtpPaths = Napi::Array::New(env);
    blockedFtpContentPatterns = Napi::Array::New(env);
    blockedHttpHeaders = Napi::Array::New(env);
    blockedHttpTrailers = Napi::Array::New(env);
    blockedHttpBodyPatterns = Napi::Array::New(env);
    blockedJsonKeys = Napi::Array::New(env);
    blockedJsonPaths = Napi::Array::New(env);
    blockedJsonValues = Napi::Array::New(env);

    policy.Set("fileFilterEnabled", Napi::Boolean::New(env, pol->FileFilterEnabled != 0));
    policy.Set("networkFilterEnabled", Napi::Boolean::New(env, pol->NetworkFilterEnabled != 0));
    policy.Set("auditEnabled", Napi::Boolean::New(env, pol->AuditEnabled != 0));
    policy.Set("fileActions", Napi::Object::New(env));
    policy.Set("networkActions", Napi::Object::New(env));
    policy.Set("processWhitelist", Napi::Array::New(env));

    AppendWStringArray(
        env,
        blockedExtensions,
        &pol->BlockedExtensions[0][0],
        pol->BlockedExtCount,
        PS_MAX_BLOCK_EXT,
        PS_EXT_LEN);
    AppendWStringArray(
        env,
        processBlacklist,
        &pol->BlockedProcesses[0][0],
        pol->BlockedProcCount,
        PS_MAX_BLOCK_PROC,
        PS_PROC_LEN);
    AppendWStringArray(
        env,
        blockedDomains,
        &pol->BlockedDomains[0][0],
        pol->BlockedDomainCount,
        PS_MAX_BLOCK_DOMAIN,
        PS_DOMAIN_LEN);
    AppendWStringArray(
        env,
        blockedUrls,
        &pol->BlockedUrls[0][0],
        pol->BlockedUrlCount,
        PS_MAX_BLOCK_URL,
        PS_URL_LEN);
    AppendWStringArray(
        env,
        blockedFtpCommands,
        &pol->BlockedFtpCommands[0][0],
        pol->BlockedFtpCommandCount,
        PS_MAX_BLOCK_FTP_CMD,
        PS_FTP_CMD_LEN);
    AppendWStringArray(
        env,
        blockedFtpPaths,
        &pol->BlockedFtpPaths[0][0],
        pol->BlockedFtpPathCount,
        PS_MAX_BLOCK_FTP_PATH,
        PS_FTP_PATH_LEN);
    AppendWStringArray(
        env,
        blockedFtpContentPatterns,
        &pol->BlockedFtpContentPatterns[0][0],
        pol->BlockedFtpContentPatternCount,
        PS_MAX_BLOCK_FTP_CONTENT,
        PS_FTP_CONTENT_LEN);
    AppendWStringArray(
        env,
        blockedHttpHeaders,
        &pol->BlockedHttpHeaders[0][0],
        pol->BlockedHttpHeaderCount,
        PS_MAX_BLOCK_HTTP_HEADER,
        PS_HTTP_HEADER_LEN);
    AppendWStringArray(
        env,
        blockedHttpTrailers,
        &pol->BlockedHttpTrailers[0][0],
        pol->BlockedHttpTrailerCount,
        PS_MAX_BLOCK_HTTP_TRAILER,
        PS_HTTP_HEADER_LEN);
    AppendWStringArray(
        env,
        blockedHttpBodyPatterns,
        &pol->BlockedHttpBodyPatterns[0][0],
        pol->BlockedHttpBodyPatternCount,
        PS_MAX_BLOCK_HTTP_BODY,
        PS_HTTP_BODY_LEN);
    AppendWStringArray(
        env,
        blockedJsonKeys,
        &pol->BlockedJsonKeys[0][0],
        pol->BlockedJsonKeyCount,
        PS_MAX_BLOCK_JSON_KEY,
        PS_JSON_KEY_LEN);
    AppendWStringArray(
        env,
        blockedJsonPaths,
        &pol->BlockedJsonPaths[0][0],
        pol->BlockedJsonPathCount,
        PS_MAX_BLOCK_JSON_PATH,
        PS_JSON_PATH_LEN);
    AppendWStringArray(
        env,
        blockedJsonValues,
        &pol->BlockedJsonValues[0][0],
        pol->BlockedJsonValueCount,
        PS_MAX_BLOCK_JSON_VALUE,
        PS_JSON_VALUE_LEN);

    for (ULONG i = 0; i < pol->BlockedPortCount && i < PS_MAX_BLOCK_PORT; i++) {
        blockedPorts.Set(blockedPorts.Length(), Napi::Number::New(env, pol->BlockedPorts[i]));
    }

    policy.Set("blockedExtensions", blockedExtensions);
    policy.Set("fileExtensions", blockedExtensions);
    policy.Set("processBlacklist", processBlacklist);
    policy.Set("blockedPorts", blockedPorts);
    policy.Set("blockedDomains", blockedDomains);
    policy.Set("blockedUrls", blockedUrls);
    policy.Set("blockedFtpCommands", blockedFtpCommands);
    policy.Set("blockedFtpPaths", blockedFtpPaths);
    policy.Set("blockedFtpContentPatterns", blockedFtpContentPatterns);
    policy.Set("blockedHttpHeaders", blockedHttpHeaders);
    policy.Set("blockedHttpTrailers", blockedHttpTrailers);
    policy.Set("blockedHttpBodyPatterns", blockedHttpBodyPatterns);
    policy.Set("blockedJsonKeys", blockedJsonKeys);
    policy.Set("blockedJsonPaths", blockedJsonPaths);
    policy.Set("blockedJsonValues", blockedJsonValues);

    return policy;
}

Napi::Value
SetPolicy(const Napi::CallbackInfo& info)
{
    Napi::Env env = info.Env();
    PS_POLICY_DATA pol;
    POLICY_COMMAND cmd;
    Napi::Object p;
    HANDLE h;
    DWORD ret = 0;

    if (info.Length() < 1 || !info[0].IsObject()) {
        return Napi::Boolean::New(env, false);
    }

    p = info[0].As<Napi::Object>();

    ZeroMemory(&pol, sizeof(pol));
    pol.FileFilterEnabled =
        (p.Has("fileFilterEnabled") && p.Get("fileFilterEnabled").ToBoolean().Value()) ? 1 : 0;
    pol.NetworkFilterEnabled =
        (p.Has("networkFilterEnabled") && p.Get("networkFilterEnabled").ToBoolean().Value()) ? 1 : 0;
    pol.AuditEnabled =
        (p.Has("auditEnabled") && p.Get("auditEnabled").ToBoolean().Value()) ? 1 : 0;

    if (p.Has("blockedExtensions") && p.Get("blockedExtensions").IsArray()) {
        pol.BlockedExtCount = FillWStringArray(
            p.Get("blockedExtensions").As<Napi::Array>(),
            &pol.BlockedExtensions[0][0],
            PS_MAX_BLOCK_EXT,
            PS_EXT_LEN);
    }

    if (p.Has("processBlacklist") && p.Get("processBlacklist").IsArray()) {
        pol.BlockedProcCount = FillWStringArray(
            p.Get("processBlacklist").As<Napi::Array>(),
            &pol.BlockedProcesses[0][0],
            PS_MAX_BLOCK_PROC,
            PS_PROC_LEN);
    }

    if (p.Has("blockedPorts") && p.Get("blockedPorts").IsArray()) {
        Napi::Array ports = p.Get("blockedPorts").As<Napi::Array>();
        ULONG n = 0;

        for (uint32_t i = 0; i < ports.Length() && n < PS_MAX_BLOCK_PORT; i++) {
            Napi::Value v = ports.Get(i);
            if (v.IsNumber()) {
                pol.BlockedPorts[n++] = (UINT16)v.As<Napi::Number>().Uint32Value();
            }
        }
        pol.BlockedPortCount = n;
    }

    if (p.Has("blockedDomains") && p.Get("blockedDomains").IsArray()) {
        pol.BlockedDomainCount = FillWStringArray(
            p.Get("blockedDomains").As<Napi::Array>(),
            &pol.BlockedDomains[0][0],
            PS_MAX_BLOCK_DOMAIN,
            PS_DOMAIN_LEN);
    }

    if (p.Has("blockedUrls") && p.Get("blockedUrls").IsArray()) {
        pol.BlockedUrlCount = FillWStringArray(
            p.Get("blockedUrls").As<Napi::Array>(),
            &pol.BlockedUrls[0][0],
            PS_MAX_BLOCK_URL,
            PS_URL_LEN);
    }

    if (p.Has("blockedFtpCommands") && p.Get("blockedFtpCommands").IsArray()) {
        pol.BlockedFtpCommandCount = FillWStringArray(
            p.Get("blockedFtpCommands").As<Napi::Array>(),
            &pol.BlockedFtpCommands[0][0],
            PS_MAX_BLOCK_FTP_CMD,
            PS_FTP_CMD_LEN);
    }

    if (p.Has("blockedFtpPaths") && p.Get("blockedFtpPaths").IsArray()) {
        pol.BlockedFtpPathCount = FillWStringArray(
            p.Get("blockedFtpPaths").As<Napi::Array>(),
            &pol.BlockedFtpPaths[0][0],
            PS_MAX_BLOCK_FTP_PATH,
            PS_FTP_PATH_LEN);
    }

    if (p.Has("blockedFtpContentPatterns") && p.Get("blockedFtpContentPatterns").IsArray()) {
        pol.BlockedFtpContentPatternCount = FillWStringArray(
            p.Get("blockedFtpContentPatterns").As<Napi::Array>(),
            &pol.BlockedFtpContentPatterns[0][0],
            PS_MAX_BLOCK_FTP_CONTENT,
            PS_FTP_CONTENT_LEN);
    }

    if (p.Has("blockedHttpHeaders") && p.Get("blockedHttpHeaders").IsArray()) {
        pol.BlockedHttpHeaderCount = FillWStringArray(
            p.Get("blockedHttpHeaders").As<Napi::Array>(),
            &pol.BlockedHttpHeaders[0][0],
            PS_MAX_BLOCK_HTTP_HEADER,
            PS_HTTP_HEADER_LEN);
    }

    if (p.Has("blockedHttpTrailers") && p.Get("blockedHttpTrailers").IsArray()) {
        pol.BlockedHttpTrailerCount = FillWStringArray(
            p.Get("blockedHttpTrailers").As<Napi::Array>(),
            &pol.BlockedHttpTrailers[0][0],
            PS_MAX_BLOCK_HTTP_TRAILER,
            PS_HTTP_HEADER_LEN);
    }

    if (p.Has("blockedHttpBodyPatterns") && p.Get("blockedHttpBodyPatterns").IsArray()) {
        pol.BlockedHttpBodyPatternCount = FillWStringArray(
            p.Get("blockedHttpBodyPatterns").As<Napi::Array>(),
            &pol.BlockedHttpBodyPatterns[0][0],
            PS_MAX_BLOCK_HTTP_BODY,
            PS_HTTP_BODY_LEN);
    }

    if (p.Has("blockedJsonKeys") && p.Get("blockedJsonKeys").IsArray()) {
        pol.BlockedJsonKeyCount = FillWStringArray(
            p.Get("blockedJsonKeys").As<Napi::Array>(),
            &pol.BlockedJsonKeys[0][0],
            PS_MAX_BLOCK_JSON_KEY,
            PS_JSON_KEY_LEN);
    }

    if (p.Has("blockedJsonPaths") && p.Get("blockedJsonPaths").IsArray()) {
        pol.BlockedJsonPathCount = FillWStringArray(
            p.Get("blockedJsonPaths").As<Napi::Array>(),
            &pol.BlockedJsonPaths[0][0],
            PS_MAX_BLOCK_JSON_PATH,
            PS_JSON_PATH_LEN);
    }

    if (p.Has("blockedJsonValues") && p.Get("blockedJsonValues").IsArray()) {
        pol.BlockedJsonValueCount = FillWStringArray(
            p.Get("blockedJsonValues").As<Napi::Array>(),
            &pol.BlockedJsonValues[0][0],
            PS_MAX_BLOCK_JSON_VALUE,
            PS_JSON_VALUE_LEN);
    }

    ZeroMemory(&cmd, sizeof(cmd));
    cmd.CommandId = CMD_SET_POLICY;
    cmd.PolicySize = sizeof(PS_POLICY_DATA);
    memcpy(cmd.PolicyData, &pol, sizeof(PS_POLICY_DATA));

    h = OpenPolicyDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return Napi::Boolean::New(env, false);
    }

    BOOL ok = DeviceIoControl(
        h,
        IOCTL_PS_SET_POLICY,
        &cmd,
        sizeof(cmd),
        NULL,
        0,
        &ret,
        NULL);
    CloseHandle(h);

    return Napi::Boolean::New(env, ok != 0);
}

Napi::Value
ReloadPolicy(const Napi::CallbackInfo& info)
{
    Napi::Env env = info.Env();
    return Napi::Boolean::New(env, true);
}

Napi::Object
InitPolicyManagerAddon(Napi::Env env, Napi::Object exports)
{
    exports.Set("getPolicy", Napi::Function::New(env, GetPolicy));
    exports.Set("setPolicy", Napi::Function::New(env, SetPolicy));
    exports.Set("reloadPolicy", Napi::Function::New(env, ReloadPolicy));
    return exports;
}
