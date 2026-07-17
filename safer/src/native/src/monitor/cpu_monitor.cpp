/*
 * cpu_monitor.cpp - CPU usage monitor implementation
 * Uses Win32 API GetSystemTimes + GetSystemInfo
 */

#include "cpu_monitor.h"
#include <windows.h>
#include <vector>

/*++
 * GetCPUUsage
 *
 * Get CPU usage snapshot using Win32 API
 *
 * --*/
Napi::Value GetCPUUsage(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // Get processor count
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    ULONG processorCount = sysInfo.dwNumberOfProcessors;

    // Use performance counter approach:
    // GetSystemTimes' kernel time already includes idle time, so
    // total = kernel + user, busy = total - idle.
    FILETIME ftIdle, ftKernel, ftUser;
    if (!GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        // Fallback: return zero
        Napi::Object result = Napi::Object::New(env);
        result.Set("totalUsage", Napi::Number::New(env, 0.0));
        result.Set("processorCount", Napi::Number::New(env, processorCount));
        result.Set("perCore", Napi::Array::New(env, 0));
        return result;
    }

    // Combine idle/kernel/user FILETIME pairs into 64-bit counters
    ULARGE_INTEGER ulIdle, ulKernel, ulUser;
    ulIdle.LowPart = ftIdle.dwLowDateTime;
    ulIdle.HighPart = ftIdle.dwHighDateTime;
    ulKernel.LowPart = ftKernel.dwLowDateTime;
    ulKernel.HighPart = ftKernel.dwHighDateTime;
    ulUser.LowPart = ftUser.dwLowDateTime;
    ulUser.HighPart = ftUser.dwHighDateTime;

    ULONGLONG total = ulKernel.QuadPart + ulUser.QuadPart;
    ULONGLONG idle = ulIdle.QuadPart;
    double totalUsage = 0.0;
    if (total > 0) {
        totalUsage = (1.0 - (double)idle / (double)total) * 100.0;
        if (totalUsage < 0.0) totalUsage = 0.0;
        if (totalUsage > 100.0) totalUsage = 100.0;
    }

    // Build result
    Napi::Object result = Napi::Object::New(env);
    result.Set("totalUsage", Napi::Number::New(env, totalUsage));
    result.Set("processorCount", Napi::Number::New(env, processorCount));

    // For per-core, we just return the total as average for now
    // (Win32 does not provide per-core CPU via simple API)
    Napi::Array perCore = Napi::Array::New(env, processorCount);
    for (ULONG i = 0; i < processorCount; i++) {
        perCore.Set(i, Napi::Number::New(env, totalUsage));
    }
    result.Set("perCore", perCore);

    return result;
}

/*++
 * InitCpuAddon
 *
 * Initialize CPU monitor module and export API
 *
 * --*/
Napi::Object InitCpuAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("getCpuUsage", Napi::Function::New(env, GetCPUUsage));
    return exports;
}
