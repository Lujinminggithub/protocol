/*
 * disk_monitor.cpp - Disk IO monitor implementation
 * Uses PDH (Performance Data Helper) for disk performance counters
 */

#include "disk_monitor.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

static HQUERY g_pdhQuery = NULL;
static HCOUNTER g_diskReadBytesCounter = NULL;
static HCOUNTER g_diskWriteBytesCounter = NULL;
static HCOUNTER g_diskReadOpsCounter = NULL;
static HCOUNTER g_diskWriteOpsCounter = NULL;
static HCOUNTER g_diskQueueCounter = NULL;
static BOOL g_pdhInitialized = FALSE;

/*
 * InitDiskAddon
 *
 * Initialize disk monitor module (includes PDH initialization)
 */
Napi::Object InitDiskAddon(Napi::Env env, Napi::Object exports) {
    if (!g_pdhInitialized) {
        PdhOpenQuery(NULL, 0, &g_pdhQuery);
        PdhAddCounterW(g_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &g_diskReadBytesCounter);
        PdhAddCounterW(g_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &g_diskWriteBytesCounter);
        PdhAddCounterW(g_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Disk Reads/sec", 0, &g_diskReadOpsCounter);
        PdhAddCounterW(g_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Disk Writes/sec", 0, &g_diskWriteOpsCounter);
        PdhAddCounterW(g_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Current Disk Queue Length", 0, &g_diskQueueCounter);
        PdhCollectQueryData(g_pdhQuery);
        Sleep(100);
        PdhCollectQueryData(g_pdhQuery);
        g_pdhInitialized = TRUE;
    }

    exports.Set("getDiskInfo", Napi::Function::New(env, GetDiskInfo));
    return exports;
}

/*
 * GetDiskInfo
 *
 * Get disk IO statistics
 */
Napi::Value GetDiskInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_pdhInitialized) {
        Napi::Object result = Napi::Object::New(env);
        result.Set("readBytesPerSec", Napi::Number::New(env, 0));
        result.Set("writeBytesPerSec", Napi::Number::New(env, 0));
        result.Set("readOpsPerSec", Napi::Number::New(env, 0));
        result.Set("writeOpsPerSec", Napi::Number::New(env, 0));
        result.Set("queueDepth", Napi::Number::New(env, 0));
        return result;
    }

    PdhCollectQueryData(g_pdhQuery);
    Sleep(100);

    HCOUNTER counters[5] = {
        g_diskReadBytesCounter, g_diskWriteBytesCounter,
        g_diskReadOpsCounter, g_diskWriteOpsCounter,
        g_diskQueueCounter
    };

    Napi::Object result = Napi::Object::New(env);
    result.Set("readBytesPerSec", Napi::Number::New(env, 0));
    result.Set("writeBytesPerSec", Napi::Number::New(env, 0));
    result.Set("readOpsPerSec", Napi::Number::New(env, 0));
    result.Set("writeOpsPerSec", Napi::Number::New(env, 0));
    result.Set("queueDepth", Napi::Number::New(env, 0));

    for (int i = 0; i < 5; i++) {
        PDH_FMT_COUNTERVALUE value;
        DWORD status = PdhGetFormattedCounterValue(counters[i],
            PDH_FMT_DOUBLE, NULL, &value);
        if (status == ERROR_SUCCESS) {
            double val = value.doubleValue;
            switch (i) {
                case 0: result.Set("readBytesPerSec", Napi::Number::New(env, val)); break;
                case 1: result.Set("writeBytesPerSec", Napi::Number::New(env, val)); break;
                case 2: result.Set("readOpsPerSec", Napi::Number::New(env, val)); break;
                case 3: result.Set("writeOpsPerSec", Napi::Number::New(env, val)); break;
                case 4: result.Set("queueDepth", Napi::Number::New(env, val)); break;
            }
        }
    }

    return result;
}
