{
  "targets": [
    {
      "target_name": "personal_safer",
      "sources": [
        "src/native_entry.cpp",
        "src/monitor/cpu_monitor.cpp",
        "src/monitor/memory_monitor.cpp",
        "src/monitor/network_monitor.cpp",
        "src/monitor/disk_monitor.cpp",
        "src/monitor/registry_monitor.cpp",
        "src/dlp/kernel_comm.cpp",
        "src/dlp/driver_loader.cpp",
        "src/dlp/file_filter_handler.cpp",
        "src/dlp/net_filter_handler.cpp",
        "src/dlp/policy_manager.cpp",
        "src/audit/file_audit.cpp",
        "src/audit/net_audit.cpp",
        "src/common/event_buffer.cpp",
        "src/common/utils.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "src"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "NAPI_VERSION=6"
      ],
      "conditions": [
        ["OS=='win'", {
          "libraries": [
            "-lkernel32.lib",
            "-lntdll.lib",
            "-lpsapi.lib",
            "-lws2_32.lib",
            "-liphlpapi.lib",
            "-lpdh.lib",
            "-lfwpuclnt.lib"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": ["/std:c++17", "/utf-8"]
            }
          }
        }]
      ]
    }
  ]
}
