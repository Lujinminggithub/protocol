/*
 * memory_monitor.h - Memory monitor module declarations
 */

#pragma once

#include <napi.h>
#include <cstdint>

struct MemorySnapshot {
    uint64_t physTotal;
    uint64_t physUsed;
    uint64_t physAvailable;
    uint64_t virtualTotal;
    uint64_t virtualUsed;
    uint64_t virtualAvailable;
    uint64_t pageTotal;
    uint64_t pageUsed;
    uint64_t pageAvailable;
    double physPercent;
};

MemorySnapshot GetMemorySnapshot();
Napi::Object InitMemoryAddon(Napi::Env env, Napi::Object exports);
Napi::Value GetMemoryInfo(const Napi::CallbackInfo& info);
