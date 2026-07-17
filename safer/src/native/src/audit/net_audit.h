/*
 * net_audit.h - Network audit module declarations
 */

#pragma once

#include <napi.h>

Napi::Object InitNetAuditAddon(Napi::Env env, Napi::Object exports);
Napi::Value LogNetEvent(const Napi::CallbackInfo& info);
Napi::Value QueryNetLogs(const Napi::CallbackInfo& info);
Napi::Value ClearNetLogs(const Napi::CallbackInfo& info);
