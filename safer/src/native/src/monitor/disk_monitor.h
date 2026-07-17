/*
 * disk_monitor.h - Disk IO monitor module declarations
 */

#pragma once

#include <napi.h>
#include <string>
#include <cstdint>

struct DiskSnapshot {
    std::string driveName;
    uint64_t readBytesPerSec;
    uint64_t writeBytesPerSec;
    uint64_t readOpsPerSec;
    uint64_t writeOpsPerSec;
    uint32_t queueDepth;
};

Napi::Object InitDiskAddon(Napi::Env env, Napi::Object exports);
Napi::Value GetDiskInfo(const Napi::CallbackInfo& info);
