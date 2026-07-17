/*
 * file_audit.cpp - 文件审计实现
 * 独立于 DLP 的文件审计处理
 */

#include "file_audit.h"
#include <windows.h>
#include <string>
#include <vector>

// 审计日志条目
struct AuditEntry {
    uint32_t id;
    uint32_t eventType;
    std::string processName;
    std::string fileName;
    uint32_t processId;
    uint64_t timestamp;
    std::string action;
};

static std::vector<AuditEntry> g_auditLogs;
static uint32_t g_nextId = 1;

/*++
 * LogFileEvent
 *
 * 记录文件审计事件
 *
 * --*/
Napi::Value LogFileEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数: eventType, processName, fileName, processId, action
    uint32_t eventType = info[0].As<Napi::Number>().Uint32Value();
    std::string processName = info[1].As<Napi::String>().Utf8Value();
    std::string fileName = info[2].As<Napi::String>().Utf8Value();
    uint32_t processId = info[3].As<Napi::Number>().Uint32Value();
    std::string action = info[4].As<Napi::String>().Utf8Value();

    AuditEntry entry;
    entry.id = g_nextId++;
    entry.eventType = eventType;
    entry.processName = processName;
    entry.fileName = fileName;
    entry.processId = processId;
    entry.timestamp = (info.Length() >= 6 && info[5].IsNumber())
        ? static_cast<uint64_t>(info[5].As<Napi::Number>().Int64Value())
        : GetTickCount64();
    entry.action = action;

    g_auditLogs.push_back(entry);

    // 限制日志数量（最多保留10000条）
    if (g_auditLogs.size() > 10000) {
        g_auditLogs.erase(g_auditLogs.begin());
    }

    Napi::Object result = Napi::Object::New(env);
    result.Set("id", Napi::Number::New(env, entry.id));
    result.Set("saved", Napi::Boolean::New(env, true));
    return result;
}

/*++
 * QueryFileLogs
 *
 * 查询文件审计日志
 *
 * --*/
Napi::Value QueryFileLogs(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数: limit, offset, eventType (可选)
    uint32_t limit = 50;
    uint32_t offset = 0;

    if (info.Length() >= 1) {
        limit = info[0].As<Napi::Object>().Get("limit").As<Napi::Number>().Uint32Value();
    }
    if (info.Length() >= 2) {
        offset = info[1].As<Napi::Object>().Get("offset").As<Napi::Number>().Uint32Value();
    }

    Napi::Array logs = Napi::Array::New(env, 0);
    uint32_t count = 0;

    for (int32_t i = static_cast<int32_t>(g_auditLogs.size()) - 1; i >= 0 && count < limit; i--) {
        if (count < offset) {
            count++;
            continue;
        }

        const AuditEntry& entry = g_auditLogs[static_cast<size_t>(i)];
        Napi::Object log = Napi::Object::New(env);
        log.Set("id", Napi::Number::New(env, entry.id));
        log.Set("eventType", Napi::Number::New(env, entry.eventType));
        log.Set("processName", Napi::String::New(env, entry.processName));
        log.Set("fileName", Napi::String::New(env, entry.fileName));
        log.Set("processId", Napi::Number::New(env, entry.processId));
        log.Set("timestamp", Napi::Number::New(env, static_cast<double>(entry.timestamp)));
        log.Set("action", Napi::String::New(env, entry.action));

        logs.Set(logs.Length(), log);
        count++;
    }

    return logs;
}

/*++
 * ClearFileLogs
 *
 * 清除审计日志
 *
 * --*/
Napi::Value ClearFileLogs(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    g_auditLogs.clear();
    g_nextId = 1;

    Napi::Object result = Napi::Object::New(env);
    result.Set("cleared", Napi::Boolean::New(env, true));
    return result;
}

/*++
 * InitFileAuditAddon
 *
 * 初始化文件审计模块
 *
 * --*/
Napi::Object InitFileAuditAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("logEvent", Napi::Function::New(env, LogFileEvent));
    exports.Set("queryLogs", Napi::Function::New(env, QueryFileLogs));
    exports.Set("clearLogs", Napi::Function::New(env, ClearFileLogs));
    return exports;
}
