/*
 * file_filter_handler.h - File filter message handler declarations
 */

#pragma once

#include <napi.h>

Napi::Object InitFileFilterHandlerAddon(Napi::Env env, Napi::Object exports);
Napi::Value HandleFileEvent(const Napi::CallbackInfo& info);
Napi::Value SetFilePolicy(const Napi::CallbackInfo& info);
