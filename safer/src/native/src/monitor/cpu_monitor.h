/*
 * cpu_monitor.h - CPU monitor module declarations
 */

#pragma once

#include <napi.h>
#include <vector>

struct CPUSnapshot {
    double totalUsage;
    std::vector<double> perCoreUsage;
};

CPUSnapshot GetCPUSnapshot();

Napi::Object InitCpuAddon(Napi::Env env, Napi::Object exports);
Napi::Value GetCPUUsage(const Napi::CallbackInfo& info);
