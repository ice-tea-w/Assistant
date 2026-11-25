#include "file_guard/util.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <sddl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

namespace file_guard::util {

namespace {

std::string pathUtf8Impl(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

} // namespace

std::string base64Encode(const Bytes &data)
{
    static const char *table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const std::uint32_t value = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(table[(value >> 18) & 63]);
        out.push_back(table[(value >> 12) & 63]);
        out.push_back(table[(value >> 6) & 63]);
        out.push_back(table[value & 63]);
        i += 3;
    }
    if (i < data.size()) {
        const std::uint32_t value =
            (data[i] << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0u);
        out.push_back(table[(value >> 18) & 63]);
        out.push_back(table[(value >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(value >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

Bytes base64Decode(const std::string &text)
{
    static int map[256];
    static bool ready = false;
    if (!ready) {
        std::fill(std::begin(map), std::end(map), -1);
        const char *table =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            map[static_cast<unsigned char>(table[i])] = i;
        ready = true;
    }
    Bytes out;
    int buffer = 0;
    int bits = 0;
    for (char ch : text) {
        if (ch == '=')
            break;
        const int value = map[static_cast<unsigned char>(ch)];
        if (value < 0)
            continue;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string hexEncode(const Bytes &data)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::uint8_t byte : data) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

Bytes hexDecode(const std::string &hex)
{
    Bytes out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned int value = 0;
        std::sscanf(hex.c_str() + i, "%2x", &value);
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
}

std::string pathUtf8(const std::filesystem::path &path)
{
    return pathUtf8Impl(path);
}

std::filesystem::path fromUtf8(const std::string &text)
{
    const auto utf8 = reinterpret_cast<const char8_t *>(text.data());
    return std::filesystem::path(std::u8string(utf8, utf8 + text.size()));
}

std::string randomHex(std::size_t bytes)
{
    return hexEncode(crypto::randomBytes(bytes));
}

std::string readText(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

Bytes readAll(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw SecurityError(ErrorCode::Io, "无法打开文件：" + pathUtf8(path));
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const std::filesystem::path &path, const Bytes &data)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw SecurityError(ErrorCode::Io, "无法写入文件：" + pathUtf8(path));
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out)
        throw SecurityError(ErrorCode::Io, "写入文件失败（磁盘满？）");
}

void writeText(const std::filesystem::path &path, const std::string &text)
{
    writeAll(path, Bytes(text.begin(), text.end()));
}

void writeTextAtomic(const std::filesystem::path &path, const std::string &text)
{
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path tmp = path.wstring() + L".tmp";
    writeText(tmp, text);
    if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw SecurityError(ErrorCode::Io, "替换文件失败：" + pathUtf8(path));
}

std::string trim(const std::string &text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string jsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string jsonUnescape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        switch (text[++i]) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        default: out.push_back(text[i]); break;
        }
    }
    return out;
}

std::string nowIso()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

std::string envOr(const char *name, const std::string &fallback)
{
    char buffer[1024] = {};
    const DWORD length = ::GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer))
        return fallback;
    return buffer;
}

std::filesystem::path defaultHome()
{
    const std::string override = envOr("FG_HOME", "");
    if (!override.empty())
        return std::filesystem::path(override);
    const std::string local = envOr("LOCALAPPDATA", "");
    if (!local.empty())
        return std::filesystem::path(local) / "file_guard_cpp";
    return std::filesystem::path("file_guard_home");
}

Bytes dpapiProtect(const Bytes &data)
{
    DATA_BLOB input{static_cast<DWORD>(data.size()), const_cast<BYTE *>(data.data())};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"file_guard", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw SecurityError(ErrorCode::Platform, "DPAPI 加密失败");
    Bytes result(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return result;
}

Bytes dpapiUnprotect(const Bytes &data)
{
    DATA_BLOB input{static_cast<DWORD>(data.size()), const_cast<BYTE *>(data.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw SecurityError(ErrorCode::Password, "DPAPI 解密失败（换了 Windows 用户/机器？）");
    Bytes result(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return result;
}

void restrictAcl(const std::filesystem::path &path)
{
    // 仅 SYSTEM / Administrators / 文件所有者可完全控制；失败不阻断（如非 NTFS）。
    // 用 Win32 API 而非 icacls：不启动子进程、不接受 shell 注入。
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)", SDDL_REVISION_1,
            &descriptor, nullptr))
        return;
    ::SetFileSecurityW(path.wstring().c_str(), DACL_SECURITY_INFORMATION, descriptor);
    ::LocalFree(descriptor);
}

} // namespace file_guard::util
