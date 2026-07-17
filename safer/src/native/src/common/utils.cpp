/*
 * utils.cpp - 公共工具函数实现
 */

#include "utils.h"
#include <windows.h>
#include <psapi.h>
#include <codecvt>

namespace PsUtils {

std::string PsUtils::WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                                           static_cast<int>(wstr.size()),
                                           NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                        static_cast<int>(wstr.size()),
                        &str[0], size_needed, NULL, NULL);
    return str;
}

std::wstring PsUtils::Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(),
                                           static_cast<int>(str.size()),
                                           NULL, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(),
                        static_cast<int>(str.size()),
                        &wstr[0], size_needed);
    return wstr;
}

std::string PsUtils::FormatFileSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIdx = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIdx < 4) {
        size /= 1024.0;
        unitIdx++;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", size, units[unitIdx]);
    return std::string(buf);
}

std::string PsUtils::FormatDataRate(uint64_t bytesPerSec) {
    return FormatFileSize(bytesPerSec) + "/s";
}

std::wstring PsUtils::GetProcessNameById(uint32_t pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                   FALSE, pid);
    if (hProcess == NULL) return L"";

    wchar_t name[MAX_PATH];
    DWORD size = ARRAYSIZE(name);
    BOOL result = GetModuleBaseNameW(hProcess, NULL, name, size);

    CloseHandle(hProcess);

    if (!result) return L"";
    return std::wstring(name);
}

}  // namespace PsUtils
