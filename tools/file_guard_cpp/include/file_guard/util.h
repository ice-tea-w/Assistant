#pragma once

// 公共工具：字节/编码/文件/路径/时间/JSON 转义。
// 目标：不依赖 Qt、vcpkg；只用 C++20 STL + Win32。

#include "file_guard/crypto.h"
#include "file_guard/errors.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace file_guard::util {

using crypto::Bytes;

std::string base64Encode(const Bytes &data);
Bytes base64Decode(const std::string &text);
std::string hexEncode(const Bytes &data);
Bytes hexDecode(const std::string &hex);
std::string pathUtf8(const std::filesystem::path &path);
std::filesystem::path fromUtf8(const std::string &text);
std::string randomHex(std::size_t bytes);

std::string readText(const std::filesystem::path &path);
Bytes readAll(const std::filesystem::path &path);
void writeAll(const std::filesystem::path &path, const Bytes &data);
void writeText(const std::filesystem::path &path, const std::string &text);
void writeTextAtomic(const std::filesystem::path &path, const std::string &text);

std::string trim(const std::string &text);
std::vector<std::string> splitLines(const std::string &text);
std::string jsonEscape(const std::string &text);
std::string jsonUnescape(const std::string &text);
std::string nowIso();

std::string envOr(const char *name, const std::string &fallback);

// DPAPI（绑定当前 Windows 用户）
Bytes dpapiProtect(const Bytes &data);
Bytes dpapiUnprotect(const Bytes &data);
std::filesystem::path defaultHome();
void restrictAcl(const std::filesystem::path &path);

} // namespace file_guard::util
