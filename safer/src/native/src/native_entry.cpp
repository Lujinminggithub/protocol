/*
 * native_entry.cpp - N-API Module Entry Point
 * Exports all sub-modules to Node.js
 */

#include <napi.h>
#include <vector>
#include <string>

// Forward declarations for monitor sub-module initializers.
// "monitor" is a flat namespace: each of these adds its own methods
// (getCpuUsage/getMemoryInfo/...) directly onto the shared exports object.
Napi::Object InitCpuAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitMemoryAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitDiskAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitNetworkAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitRegistryAddon(Napi::Env env, Napi::Object exports);

// Forward declarations for DLP sub-module initializers.
// "dlp" is a nested namespace: each sub-module gets its own child object
// (dlp.kernel_comm, dlp.driver_loader, ...) matching the Electron-side
// addon.dlp.<name> access pattern in main/ipc/handler_registry.ts.
Napi::Object InitKernelCommAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitDriverLoaderAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitFileFilterHandlerAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitNetFilterHandlerAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitPolicyManagerAddon(Napi::Env env, Napi::Object exports);

// Forward declarations for audit sub-module initializers.
// "audit" is also a nested namespace (audit.file_audit, audit.net_audit).
Napi::Object InitFileAuditAddon(Napi::Env env, Napi::Object exports);
Napi::Object InitNetAuditAddon(Napi::Env env, Napi::Object exports);

/*
 * InitMonitorAddon
 *
 * Merges all monitor sub-modules into a single flat "monitor" object.
 */
Napi::Object InitMonitorAddon(Napi::Env env, Napi::Object exports) {
    InitCpuAddon(env, exports);
    InitMemoryAddon(env, exports);
    InitDiskAddon(env, exports);
    InitNetworkAddon(env, exports);
    InitRegistryAddon(env, exports);
    return exports;
}

/*
 * InitDlpAddon
 *
 * Builds the "dlp" object with one child object per sub-module.
 */
Napi::Object InitDlpAddon(Napi::Env env, Napi::Object exports) {
    Napi::Object kernelComm = Napi::Object::New(env);
    InitKernelCommAddon(env, kernelComm);
    exports.Set("kernel_comm", kernelComm);

    Napi::Object driverLoader = Napi::Object::New(env);
    InitDriverLoaderAddon(env, driverLoader);
    exports.Set("driver_loader", driverLoader);

    Napi::Object fileFilterHandler = Napi::Object::New(env);
    InitFileFilterHandlerAddon(env, fileFilterHandler);
    exports.Set("file_filter_handler", fileFilterHandler);

    Napi::Object netFilterHandler = Napi::Object::New(env);
    InitNetFilterHandlerAddon(env, netFilterHandler);
    exports.Set("net_filter_handler", netFilterHandler);

    Napi::Object policyManager = Napi::Object::New(env);
    InitPolicyManagerAddon(env, policyManager);
    exports.Set("policy_manager", policyManager);

    return exports;
}

/*
 * InitAuditAddon
 *
 * Builds the "audit" object with one child object per sub-module.
 */
Napi::Object InitAuditAddon(Napi::Env env, Napi::Object exports) {
    Napi::Object fileAudit = Napi::Object::New(env);
    InitFileAuditAddon(env, fileAudit);
    exports.Set("file_audit", fileAudit);

    Napi::Object netAudit = Napi::Object::New(env);
    InitNetAuditAddon(env, netAudit);
    exports.Set("net_audit", netAudit);

    return exports;
}

// Addon registry
struct AddonRegistry {
    const char* name;
    Napi::Object (*init)(Napi::Env, Napi::Object);
};

static const std::vector<AddonRegistry> addons = {
    {"monitor", InitMonitorAddon},
    {"dlp", InitDlpAddon},
    {"audit", InitAuditAddon}
};

/*
 * Init - N-API module entry point
 * Initializes all sub-modules and exports them
 */
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    for (const auto& addon : addons) {
        Napi::Object addonExports = addon.init(env, Napi::Object::New(env));
        exports.Set(addon.name, addonExports);
    }

    return exports;
}

// Register the module
NODE_API_MODULE(personal_safer, Init)
