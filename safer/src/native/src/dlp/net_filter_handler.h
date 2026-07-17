/*
 * net_filter_handler.h - Network filter message handler declarations
 */

#pragma once

#include <napi.h>

Napi::Object InitNetFilterHandlerAddon(Napi::Env env, Napi::Object exports);
Napi::Value HandleNetEvent(const Napi::CallbackInfo& info);
Napi::Value SetNetPolicy(const Napi::CallbackInfo& info);
