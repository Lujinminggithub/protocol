/*
 * kernel_comm.h - Kernel communication module declarations
 * Handles IOCTL and named pipe communication with kernel driver
 */

#pragma once

#include <napi.h>
#include <winsock2.h>
#include <windows.h>
#include <string>

struct KernelCommState {
    HANDLE deviceHandle;
    HANDLE pipeHandle;
    bool isConnected;
    bool canControl;
    bool isPipeConnected;
    DWORD lastErrorCode;
    std::string lastErrorStage;
    std::string lastError;

    KernelCommState() : deviceHandle(INVALID_HANDLE_VALUE),
                        pipeHandle(INVALID_HANDLE_VALUE),
                        isConnected(false), canControl(false),
                        isPipeConnected(false), lastErrorCode(ERROR_SUCCESS) {}
};

Napi::Object InitKernelCommAddon(Napi::Env env, Napi::Object exports);
Napi::Value ConnectToDevice(const Napi::CallbackInfo& info);
Napi::Value DisconnectFromDevice(const Napi::CallbackInfo& info);
Napi::Value GetConnectionState(const Napi::CallbackInfo& info);
Napi::Value SendIoctl(const Napi::CallbackInfo& info);
Napi::Value GetDriverStatus(const Napi::CallbackInfo& info);
Napi::Value WaitForEvents(const Napi::CallbackInfo& info);
Napi::Value SetQuarantineConfig(const Napi::CallbackInfo& info);
Napi::Value GetQuarantineConfig(const Napi::CallbackInfo& info);
Napi::Value SetRedirectConfig(const Napi::CallbackInfo& info);
Napi::Value QueryRedirectDestination(const Napi::CallbackInfo& info);
Napi::Value LookupConnectionProcess(const Napi::CallbackInfo& info);
Napi::Value ReadFileEvent(const Napi::CallbackInfo& info);
Napi::Value ReadFileEventsBatch(const Napi::CallbackInfo& info);
Napi::Value ReadNetEvent(const Napi::CallbackInfo& info);
Napi::Value ReadNetEventsBatch(const Napi::CallbackInfo& info);
