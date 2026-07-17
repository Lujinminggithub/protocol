/*
 * memory_monitor.cpp - Memory monitor implementation
 * Uses GlobalMemoryStatusEx + GetPerformanceInfo
 */

#include "memory_monitor.h"
#include <windows.h>
#include <psapi.h>

/*
 * GetMemoryInfo
 *
 * Get memory usage snapshot
 */
Napi::Value GetMemoryInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (!GlobalMemoryStatusEx(&memStatus)) {
        return Napi::Object::New(env);
    }

    PERFORMANCE_INFORMATION perfInfo;
    if (!GetPerformanceInfo(&perfInfo, sizeof(perfInfo))) {
        return Napi::Object::New(env);
    }

    Napi::Object result = Napi::Object::New(env);
    result.Set("physTotal", Napi::Number::New(env, (double)memStatus.ullTotalPhys));
    result.Set("physUsed", Napi::Number::New(env,
        (double)(memStatus.ullTotalPhys - memStatus.ullAvailPhys)));
    result.Set("physAvailable", Napi::Number::New(env, (double)memStatus.ullAvailPhys));
    result.Set("virtualTotal", Napi::Number::New(env, (double)memStatus.ullTotalVirtual));
    result.Set("virtualUsed", Napi::Number::New(env,
        (double)(memStatus.ullTotalVirtual - memStatus.ullAvailVirtual)));
    result.Set("virtualAvailable", Napi::Number::New(env, (double)memStatus.ullAvailVirtual));
    result.Set("pageTotal", Napi::Number::New(env, (double)memStatus.ullTotalPageFile));
    result.Set("pageUsed", Napi::Number::New(env,
        (double)(memStatus.ullTotalPageFile - memStatus.ullAvailPageFile)));
    result.Set("pageAvailable", Napi::Number::New(env, (double)memStatus.ullAvailPageFile));

    double physPercent = 0.0;
    if (memStatus.ullTotalPhys > 0) {
        physPercent = (double)(memStatus.ullTotalPhys - memStatus.ullAvailPhys) /
                      (double)memStatus.ullTotalPhys * 100.0;
    }
    result.Set("physPercent", Napi::Number::New(env, physPercent));

    return result;
}

/*
 * InitMemoryAddon
 *
 * Initialize memory monitor module
 */
Napi::Object InitMemoryAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("getMemoryInfo", Napi::Function::New(env, GetMemoryInfo));
    return exports;
}
