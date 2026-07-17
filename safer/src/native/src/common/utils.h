/*
 * utils.h - Common utility function declarations
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace PsUtils {

std::string WideToUtf8(const std::wstring& wstr);
std::wstring Utf8ToWide(const std::string& str);
std::string FormatFileSize(uint64_t bytes);
std::string FormatDataRate(uint64_t bytesPerSec);
std::wstring GetProcessNameById(uint32_t pid);

}  // namespace PsUtils
