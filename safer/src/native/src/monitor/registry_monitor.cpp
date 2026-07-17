/*
 * registry_monitor.cpp - Registry monitor implementation
 * Uses RegNotifyChangeKeyValue for async registry change notification
 */

#include "registry_monitor.h"
#include <windows.h>

static HANDLE g_regWatchEvent = NULL;
static HANDLE g_watchThread = NULL;
static WCHAR g_watchedKeyPath[MAX_PATH] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer";
static BOOL g_isWatching = FALSE;

/*
 * RegistryWatchThread
 *
 * Registry monitoring thread
 */
DWORD WINAPI RegistryWatchThread(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);

    while (g_isWatching) {
        DWORD result = RegNotifyChangeKeyValue(
            HKEY_CURRENT_USER,
            TRUE,
            REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
            g_regWatchEvent,
            TRUE
        );

        if (result == ERROR_SUCCESS) {
            WaitForSingleObject(g_regWatchEvent, INFINITE);
            ResetEvent(g_regWatchEvent);
        }
    }
    return ERROR_SUCCESS;
}

/*
 * WatchRegistryKey
 *
 * Start monitoring a registry key
 */
Napi::Value WatchRegistryKey(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (g_isWatching) {
        return Napi::Boolean::New(env, false);
    }

    g_regWatchEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_isWatching = TRUE;

    g_watchThread = CreateThread(NULL, 0, RegistryWatchThread, NULL, 0, NULL);

    return Napi::Boolean::New(env, true);
}

/*
 * UnwatchRegistryKey
 *
 * Stop monitoring a registry key
 */
Napi::Value UnwatchRegistryKey(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    g_isWatching = FALSE;
    if (g_regWatchEvent) {
        SetEvent(g_regWatchEvent);
        CloseHandle(g_regWatchEvent);
        g_regWatchEvent = NULL;
    }
    if (g_watchThread) {
        CloseHandle(g_watchThread);
        g_watchThread = NULL;
    }

    return Napi::Boolean::New(env, true);
}

/*
 * InitRegistryAddon
 *
 * Initialize registry monitor module
 */
Napi::Object InitRegistryAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("watchKey", Napi::Function::New(env, WatchRegistryKey));
    exports.Set("unwatchKey", Napi::Function::New(env, UnwatchRegistryKey));
    return exports;
}
