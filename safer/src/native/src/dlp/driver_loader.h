/*
 * driver_loader.h - Driver load/unload module declarations
 */

#pragma once

#include <napi.h>

Napi::Object InitDriverLoaderAddon(Napi::Env env, Napi::Object exports);
Napi::Value LoadDriver(const Napi::CallbackInfo& info);
Napi::Value UnloadDriver(const Napi::CallbackInfo& info);
Napi::Value IsDriverLoaded(const Napi::CallbackInfo& info);
