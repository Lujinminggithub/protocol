/*
 * policy_manager.h - Policy management module declarations
 */

#pragma once

#include <napi.h>

Napi::Object InitPolicyManagerAddon(Napi::Env env, Napi::Object exports);
Napi::Value GetPolicy(const Napi::CallbackInfo& info);
Napi::Value SetPolicy(const Napi::CallbackInfo& info);
Napi::Value ReloadPolicy(const Napi::CallbackInfo& info);
