/*
 * registry_monitor.cpp - real registry change notification bridge
 */

#include "registry_monitor.h"
#include <windows.h>
#include <deque>
#include <mutex>
#include <string>

struct RegistryChangeEvent {
    ULONGLONG timestampMs;
    std::wstring keyPath;
    std::wstring changeType;
};

static HANDLE g_regWatchEvent = NULL;
static HANDLE g_watchThread = NULL;
static HKEY g_watchedKey = NULL;
static volatile LONG g_isWatching = FALSE;
static std::wstring g_watchedKeyPath;
static std::deque<RegistryChangeEvent> g_events;
static std::mutex g_eventsMutex;
static const size_t kMaxRegistryEvents = 2048;

static ULONGLONG UnixTimeMilliseconds() {
    FILETIME fileTime;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&fileTime);
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return (value.QuadPart - 116444736000000000ULL) / 10000ULL;
}

static void PushChangeEvent() {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    g_events.push_back({ UnixTimeMilliseconds(), g_watchedKeyPath, L"key_or_value_changed" });
    while (g_events.size() > kMaxRegistryEvents) g_events.pop_front();
}

static bool ParseRegistryPath(const std::wstring& value, HKEY* root, std::wstring* subKey) {
    struct Prefix { const wchar_t* text; HKEY root; };
    static const Prefix prefixes[] = {
        { L"HKEY_CURRENT_USER\\", HKEY_CURRENT_USER }, { L"HKCU\\", HKEY_CURRENT_USER },
        { L"HKEY_LOCAL_MACHINE\\", HKEY_LOCAL_MACHINE }, { L"HKLM\\", HKEY_LOCAL_MACHINE },
    };
    for (const auto& prefix : prefixes) {
        const size_t length = wcslen(prefix.text);
        if (_wcsnicmp(value.c_str(), prefix.text, length) == 0 && value.size() > length) {
            *root = prefix.root;
            *subKey = value.substr(length);
            return true;
        }
    }
    return false;
}

static DWORD WINAPI RegistryWatchThread(LPVOID) {
    while (InterlockedCompareExchange(&g_isWatching, FALSE, FALSE)) {
        const LSTATUS result = RegNotifyChangeKeyValue(
            g_watchedKey,
            TRUE,
            REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_ATTRIBUTES |
                REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_SECURITY,
            g_regWatchEvent,
            TRUE);
        if (result != ERROR_SUCCESS) break;

        const DWORD waitResult = WaitForSingleObject(g_regWatchEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0 ||
            !InterlockedCompareExchange(&g_isWatching, FALSE, FALSE)) break;
        ResetEvent(g_regWatchEvent);
        PushChangeEvent();
    }
    InterlockedExchange(&g_isWatching, FALSE);
    return ERROR_SUCCESS;
}

static void StopWatcher() {
    InterlockedExchange(&g_isWatching, FALSE);
    if (g_regWatchEvent) SetEvent(g_regWatchEvent);
    if (g_watchThread) {
        WaitForSingleObject(g_watchThread, 5000);
        CloseHandle(g_watchThread);
        g_watchThread = NULL;
    }
    if (g_watchedKey) {
        RegCloseKey(g_watchedKey);
        g_watchedKey = NULL;
    }
    if (g_regWatchEvent) {
        CloseHandle(g_regWatchEvent);
        g_regWatchEvent = NULL;
    }
}

Napi::Value WatchRegistryKey(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (InterlockedCompareExchange(&g_isWatching, FALSE, FALSE)) {
        Napi::Error::New(env, "registry watcher is already running").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "registry path is required").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const std::u16string input = info[0].As<Napi::String>().Utf16Value();
    const std::wstring path(reinterpret_cast<const wchar_t*>(input.data()), input.size());
    HKEY root = NULL;
    std::wstring subKey;
    if (!ParseRegistryPath(path, &root, &subKey)) {
        Napi::RangeError::New(env, "registry path must start with HKCU or HKLM").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const LSTATUS openResult = RegOpenKeyExW(root, subKey.c_str(), 0, KEY_NOTIFY, &g_watchedKey);
    if (openResult != ERROR_SUCCESS) {
        Napi::Error::New(env, "cannot open registry key: " + std::to_string(openResult))
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    g_regWatchEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_regWatchEvent) {
        StopWatcher();
        Napi::Error::New(env, "cannot create registry notification event").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    g_watchedKeyPath = path;
    InterlockedExchange(&g_isWatching, TRUE);
    g_watchThread = CreateThread(NULL, 0, RegistryWatchThread, NULL, 0, NULL);
    if (!g_watchThread) {
        StopWatcher();
        Napi::Error::New(env, "cannot create registry watcher thread").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    return Napi::Boolean::New(env, true);
}

Napi::Value UnwatchRegistryKey(const Napi::CallbackInfo& info) {
    StopWatcher();
    return Napi::Boolean::New(info.Env(), true);
}

Napi::Value ReadRegistryEvents(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    uint32_t limit = 200;
    if (info.Length() > 0 && info[0].IsNumber()) limit = info[0].As<Napi::Number>().Uint32Value();
    if (limit > 1000) limit = 1000;

    Napi::Array output = Napi::Array::New(env);
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    uint32_t index = 0;
    while (!g_events.empty() && index < limit) {
        RegistryChangeEvent event = std::move(g_events.front());
        g_events.pop_front();
        Napi::Object item = Napi::Object::New(env);
        item.Set("timestampMs", Napi::Number::New(env, static_cast<double>(event.timestampMs)));
        item.Set("keyPath", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(event.keyPath.data()), event.keyPath.size()));
        item.Set("changeType", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(event.changeType.data()), event.changeType.size()));
        output.Set(index++, item);
    }
    return output;
}

Napi::Object InitRegistryAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("watchKey", Napi::Function::New(env, WatchRegistryKey));
    exports.Set("unwatchKey", Napi::Function::New(env, UnwatchRegistryKey));
    exports.Set("readEvents", Napi::Function::New(env, ReadRegistryEvents));
    return exports;
}
