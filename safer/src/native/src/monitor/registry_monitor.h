/*
 * registry_monitor.h - Registry monitor module declarations
 */

#pragma once

#include <napi.h>

Napi::Object InitRegistryAddon(Napi::Env env, Napi::Object exports);
Napi::Value WatchRegistryKey(const Napi::CallbackInfo& info);
Napi::Value UnwatchRegistryKey(const Napi::CallbackInfo& info);
