/*
 * file_filter_handler.cpp - 文件过滤消息处理实现
 */

#include "file_filter_handler.h"
#include <windows.h>
#include <string>

/*++
 * HandleFileEvent
 *
 * 处理从内核收到的文件事件
 *
 * --*/
Napi::Value HandleFileEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数: eventType, processId, processName, fileName, action
    uint32_t eventType = info[0].As<Napi::Number>().Uint32Value();
    uint32_t processId = info[1].As<Napi::Number>().Uint32Value();
    std::string processName = info[2].As<Napi::String>().Utf8Value();
    std::string fileName = info[3].As<Napi::String>().Utf8Value();
    uint32_t action = info[4].As<Napi::Number>().Uint32Value();

    // 构建事件对象
    Napi::Object event = Napi::Object::New(env);
    event.Set("eventType", Napi::Number::New(env, eventType));
    event.Set("processId", Napi::Number::New(env, processId));
    event.Set("processName", Napi::String::New(env, processName));
    event.Set("fileName", Napi::String::New(env, fileName));
    event.Set("action", Napi::Number::New(env, action));
    event.Set("timestamp", Napi::Number::New(env,
        static_cast<double>(GetTickCount64())));

    // TODO: 广播到 DLP handler 和 Audit handler
    // 通过 EventEmitter 发送到 Electron 主进程

    return event;
}

/*++
 * SetFilePolicy
 *
 * 设置文件过滤策略
 *
 * --*/
Napi::Value SetFilePolicy(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数: policy (JSON object)
    Napi::Object policyObj = info[0].As<Napi::Object>();

    // TODO: 通过 IOCTL 将策略发送到内核
    // IOCTL_PS_SET_POLICY

    return Napi::Boolean::New(env, true);
}

/*++
 * InitFileFilterHandlerAddon
 *
 * 初始化文件过滤处理模块
 *
 * --*/
Napi::Object InitFileFilterHandlerAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("handleEvent", Napi::Function::New(env, HandleFileEvent));
    exports.Set("setPolicy", Napi::Function::New(env, SetFilePolicy));
    return exports;
}
