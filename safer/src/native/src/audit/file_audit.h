/*
 * file_audit.h - File audit module declarations
 */

#pragma once

#include <napi.h>

Napi::Object InitFileAuditAddon(Napi::Env env, Napi::Object exports);
Napi::Value LogFileEvent(const Napi::CallbackInfo& info);
Napi::Value QueryFileLogs(const Napi::CallbackInfo& info);
Napi::Value ClearFileLogs(const Napi::CallbackInfo& info);
