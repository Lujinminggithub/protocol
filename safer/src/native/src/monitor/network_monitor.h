/*
 * network_monitor.h - Network monitor module declarations
 */

#pragma once

#include <napi.h>
#include <vector>
#include <string>
#include <cstdint>

struct NetworkInterfaceInfo {
    std::string name;
    std::string displayName;
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint64_t packetsReceived;
    uint64_t packetsSent;
    uint32_t tcpConnections;
    uint32_t udpListeners;
};

Napi::Object InitNetworkAddon(Napi::Env env, Napi::Object exports);
Napi::Value GetNetworkInfo(const Napi::CallbackInfo& info);
