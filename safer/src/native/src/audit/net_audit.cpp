/*
 * net_audit.cpp - 网络审计实现
 */

#include "net_audit.h"
#include <windows.h>
#include <string>
#include <vector>

struct NetAuditEntry {
    uint32_t id;
    uint32_t eventType;
    std::string processName;
    std::string url;
    uint32_t processId;
    uint16_t remotePort;
    uint16_t localPort;
    uint8_t protocol;
    uint64_t timestamp;
    std::string action;
};

static std::vector<NetAuditEntry> g_netLogs;
static uint32_t g_nextNetId = 1;

/*++
 * LogNetEvent
 *
 * 记录网络审计事件
 *
 * --*/
Napi::Value LogNetEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    NetAuditEntry entry;
    entry.id = g_nextNetId++;
    entry.eventType = info[0].As<Napi::Number>().Uint32Value();
    entry.processName = info[1].As<Napi::String>().Utf8Value();
    entry.url = info[2].As<Napi::String>().Utf8Value();
    entry.processId = info[3].As<Napi::Number>().Uint32Value();
    entry.remotePort = static_cast<uint16_t>(info[4].As<Napi::Number>().Uint32Value());
    entry.localPort = static_cast<uint16_t>(info[5].As<Napi::Number>().Uint32Value());
    entry.protocol = static_cast<uint8_t>(info[6].As<Napi::Number>().Uint32Value());
    entry.timestamp = (info.Length() >= 9 && info[8].IsNumber())
        ? static_cast<uint64_t>(info[8].As<Napi::Number>().Int64Value())
        : GetTickCount64();
    entry.action = info[7].As<Napi::String>().Utf8Value();

    g_netLogs.push_back(entry);

    if (g_netLogs.size() > 10000) {
        g_netLogs.erase(g_netLogs.begin());
    }

    Napi::Object result = Napi::Object::New(env);
    result.Set("id", Napi::Number::New(env, entry.id));
    result.Set("saved", Napi::Boolean::New(env, true));
    return result;
}

/*++
 * QueryNetLogs
 *
 * 查询网络审计日志
 *
 * --*/
Napi::Value QueryNetLogs(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    Napi::Array logs = Napi::Array::New(env, 0);
    uint32_t limit = 50;

    if (info.Length() >= 1) {
        limit = info[0].As<Napi::Object>().Get("limit").As<Napi::Number>().Uint32Value();
    }

    uint32_t count = 0;
    for (int32_t i = static_cast<int32_t>(g_netLogs.size()) - 1; i >= 0 && count < limit; i--) {
        const NetAuditEntry& entry = g_netLogs[static_cast<size_t>(i)];
        Napi::Object log = Napi::Object::New(env);
        log.Set("id", Napi::Number::New(env, entry.id));
        log.Set("eventType", Napi::Number::New(env, entry.eventType));
        log.Set("processName", Napi::String::New(env, entry.processName));
        log.Set("url", Napi::String::New(env, entry.url));
        log.Set("processId", Napi::Number::New(env, entry.processId));
        log.Set("remotePort", Napi::Number::New(env, entry.remotePort));
        log.Set("localPort", Napi::Number::New(env, entry.localPort));
        log.Set("protocol", Napi::Number::New(env, entry.protocol));
        log.Set("timestamp", Napi::Number::New(env, static_cast<double>(entry.timestamp)));
        log.Set("action", Napi::String::New(env, entry.action));
        logs.Set(logs.Length(), log);
        count++;
    }

    return logs;
}

/*++
 * ClearNetLogs
 *
 * 清除网络审计日志
 *
 * --*/
Napi::Value ClearNetLogs(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    g_netLogs.clear();
    g_nextNetId = 1;

    Napi::Object result = Napi::Object::New(env);
    result.Set("cleared", Napi::Boolean::New(env, true));
    return result;
}

/*++
 * InitNetAuditAddon
 *
 * 初始化网络审计模块
 *
 * --*/
Napi::Object InitNetAuditAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("logEvent", Napi::Function::New(env, LogNetEvent));
    exports.Set("queryLogs", Napi::Function::New(env, QueryNetLogs));
    exports.Set("clearLogs", Napi::Function::New(env, ClearNetLogs));
    return exports;
}
