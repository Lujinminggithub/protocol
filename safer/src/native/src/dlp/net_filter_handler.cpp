/*
 * net_filter_handler.cpp - 网络过滤消息处理实现
 */

#include "net_filter_handler.h"
#include <windows.h>
#include <string>

/*++
 * HandleNetEvent
 *
 * 处理从内核收到的网络事件
 *
 * --*/
Napi::Value HandleNetEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数: eventType, processId, processName, url, protocol, port, action
    uint32_t eventType = info[0].As<Napi::Number>().Uint32Value();
    uint32_t processId = info[1].As<Napi::Number>().Uint32Value();
    std::string processName = info[2].As<Napi::String>().Utf8Value();
    std::string url = info[3].As<Napi::String>().Utf8Value();
    uint32_t protocol = info[4].As<Napi::Number>().Uint32Value();
    uint32_t port = info[5].As<Napi::Number>().Uint32Value();
    uint32_t action = info[6].As<Napi::Number>().Uint32Value();

    Napi::Object event = Napi::Object::New(env);
    event.Set("eventType", Napi::Number::New(env, eventType));
    event.Set("processId", Napi::Number::New(env, processId));
    event.Set("processName", Napi::String::New(env, processName));
    event.Set("url", Napi::String::New(env, url));
    event.Set("protocol", Napi::Number::New(env, protocol));
    event.Set("port", Napi::Number::New(env, port));
    event.Set("action", Napi::Number::New(env, action));
    event.Set("timestamp", Napi::Number::New(env,
        static_cast<double>(GetTickCount64())));

    return event;
}

/*++
 * SetNetPolicy
 *
 * 设置网络过滤策略
 *
 * --*/
Napi::Value SetNetPolicy(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    Napi::Object policyObj = info[0].As<Napi::Object>();

    // TODO: 通过 IOCTL 将策略发送到内核
    return Napi::Boolean::New(env, true);
}

/*++
 * InitNetFilterHandlerAddon
 *
 * 初始化网络过滤处理模块
 *
 * --*/
Napi::Object InitNetFilterHandlerAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("handleEvent", Napi::Function::New(env, HandleNetEvent));
    exports.Set("setPolicy", Napi::Function::New(env, SetNetPolicy));
    return exports;
}
