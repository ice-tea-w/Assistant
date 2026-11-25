#include "file_guard/scan.h"

#include "file_guard/zipx.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

namespace file_guard::scan {

namespace {

std::string pathUtf8(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

constexpr std::size_t kMaxTextBytes = 8u << 20;

char severityChar(Severity severity)
{
    switch (severity) {
    case Severity::High: return 'H';
    case Severity::Medium: return 'M';
    case Severity::Low: return 'L';
    default: return 'I';
    }
}

bool isReparsePoint(const std::filesystem::path &path)
{
    const DWORD attrs = ::GetFileAttributesW(path.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return true;
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool isCloudPlaceholder(const std::filesystem::path &path)
{
    const DWORD attrs = ::GetFileAttributesW(path.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return false;
    constexpr DWORD kOffline = 0x1000;
    constexpr DWORD kRecallOnOpen = 0x40000;
    constexpr DWORD kRecallOnDataAccess = 0x400000;
    return (attrs & (kOffline | kRecallOnOpen | kRecallOnDataAccess)) != 0;
}

bool isBinaryText(const std::string &data)
{
    if (data.empty())
        return false;
    std::size_t control = 0;
    for (std::size_t i = 0; i < data.size() && i < 8192; ++i) {
        const unsigned char byte = static_cast<unsigned char>(data[i]);
        if (byte == 0)
            return true;
        if (byte < 0x09)
            ++control;
    }
    return control > 8;
}

bool isOfficeExtension(const std::string &extension)
{
    static const char *exts[] = {".docx", ".docm", ".xlsx", ".xlsm", ".pptx", ".pptm"};
    for (const char *ext : exts) {
        if (extension == ext)
            return true;
    }
    return false;
}

std::string readFileText(const std::filesystem::path &path, std::size_t *sizeOut, std::string *reason)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        *reason = "无法读取文件大小";
        return {};
    }
    *sizeOut = static_cast<std::size_t>(size);
    if (size > kMaxTextBytes) {
        *reason = "文件过大，跳过内容读取";
        return {};
    }

    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (isOfficeExtension(extension)) {
        const std::string text = readOfficeText(path);
        if (text.empty())
            *reason = "Office 正文抽取失败/为空";
        return text;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *reason = "无法打开文件";
        return {};
    }
    std::ostringstream stream;
    stream << in.rdbuf();
    std::string text = stream.str();
    if (isBinaryText(text)) {
        *reason = "二进制文件，跳过内容读取";
        return {};
    }
    return text;
}

void addFinding(std::vector<Finding> &out, const std::string &rule, Severity severity,
                const std::string &sample, std::size_t line)
{
    for (const Finding &existing : out) {
        if (existing.rule == rule && existing.sample == sample)
            return;
    }
    out.push_back(Finding{rule, severity, sample, line});
}

std::size_t lineOf(const std::string &text, std::size_t offset)
{
    return static_cast<std::size_t>(std::count(text.begin(), text.begin() + offset, '\n')) + 1;
}

} // namespace

std::string maskValue(const std::string &value)
{
    if (value.size() <= 4)
        return std::string(value.size(), '*');
    std::string out = value;
    for (std::size_t i = 1; i + 1 < out.size(); ++i)
        out[i] = '*';
    return out;
}

bool isValidLuhn(const std::string &digits)
{
    if (digits.size() < 12 || digits.size() > 19)
        return false;
    int sum = 0;
    bool doubled = false;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (!std::isdigit(static_cast<unsigned char>(*it)))
            return false;
        int value = *it - '0';
        if (doubled) {
            value *= 2;
            if (value > 9)
                value -= 9;
        }
        sum += value;
        doubled = !doubled;
    }
    return sum % 10 == 0;
}

bool isValidIdCard(const std::string &digits)
{
    if (digits.size() != 18)
        return false;
    static const int weights[17] = {7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2};
    static const char checks[11] = {'1', '0', 'X', '9', '8', '7', '6', '5', '4', '3', '2'};
    int sum = 0;
    for (int i = 0; i < 17; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(digits[static_cast<std::size_t>(i)])))
            return false;
        sum += (digits[static_cast<std::size_t>(i)] - '0') * weights[i];
    }
    const char last = static_cast<char>(std::toupper(static_cast<unsigned char>(digits[17])));
    return checks[sum % 11] == last;
}

bool isValidUscc(const std::string &code)
{
    if (code.size() != 18)
        return false;
    static const char *alphabet = "0123456789ABCDEFGHJKLMNPQRTUWXY";
    static const int weights[17] = {1, 3, 9, 27, 19, 26, 16, 17, 20, 29, 25, 13, 8, 24, 10, 30, 28};
    int sum = 0;
    for (int i = 0; i < 17; ++i) {
        const char ch = code[static_cast<std::size_t>(i)];
        const char *found = std::strchr(alphabet, ch);
        if (!found)
            return false;
        sum += static_cast<int>(found - alphabet) * weights[i];
    }
    const int expected = (31 - sum % 31) % 31;
    const char last = code[17];
    const char *found = std::strchr(alphabet, last);
    return found && static_cast<int>(found - alphabet) == expected;
}

std::vector<Finding> scanText(const std::string &text, const std::string &filename)
{
    std::vector<Finding> findings;

    // 1) 文件名/正文关键词（低危）
    if (filename.find("合同") != std::string::npos)
        addFinding(findings, "filename.contract", Severity::Low, filename, 0);
    for (const char *keyword : {"保密", "机密", "绝密"}) {
        const std::size_t pos = text.find(keyword);
        if (pos != std::string::npos)
            addFinding(findings, "keyword.classified", Severity::Medium, keyword, lineOf(text, pos));
    }

    // 2) 身份证（校验位）
    {
        static const std::regex re(R"(([1-9]\d{5}(?:19|20)\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])\d{3}[\dXx]))");
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
            const std::string value = it->str(1);
            if (isValidIdCard(value))
                addFinding(findings, "id_card", Severity::High, maskValue(value),
                           lineOf(text, static_cast<std::size_t>(it->position())));
        }
    }

    // 3) 银行卡（Luhn）
    {
        static const std::regex re(R"(\b(\d{16,19})\b)");
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
            const std::string value = it->str(1);
            if (isValidLuhn(value))
                addFinding(findings, "bank_card", Severity::High, maskValue(value),
                           lineOf(text, static_cast<std::size_t>(it->position())));
        }
    }

    // 4) 手机号
    {
        static const std::regex re(R"(\b(1[3-9]\d{9})\b)");
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
            addFinding(findings, "phone", Severity::Low, maskValue(it->str(1)),
                       lineOf(text, static_cast<std::size_t>(it->position())));
    }

    // 5) 统一社会信用代码（mod 31）
    {
        static const std::regex re(R"(\b([0-9A-HJ-NPQRTUWXY]{18})\b)");
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
            const std::string value = it->str(1);
            if (isValidUscc(value))
                addFinding(findings, "uscc", Severity::Medium, maskValue(value),
                           lineOf(text, static_cast<std::size_t>(it->position())));
        }
    }

    return findings;
}

std::string readOfficeText(const std::filesystem::path &path)
{
    constexpr std::size_t kMaxEntry = 4u << 20;
    constexpr std::size_t kMaxTotal = 8u << 20;
    std::string text;
    std::size_t total = 0;
    int used = 0;
    try {
        const auto entries = zipx::list(path);
        for (const auto &entry : entries) {
            if (used >= 50 || total >= kMaxTotal)
                break;
            const bool wanted =
                (entry.name.rfind("word/", 0) == 0 || entry.name.rfind("xl/", 0) == 0 ||
                 entry.name.rfind("ppt/slides/", 0) == 0) &&
                entry.name.size() > 4 && entry.name.substr(entry.name.size() - 4) == ".xml";
            if (!wanted || entry.uncompSize > kMaxEntry)
                continue;
            const crypto::Bytes raw = zipx::readEntry(path, entry, kMaxEntry);
            if (raw.empty())
                continue;
            total += raw.size();
            ++used;
            // 粗粒度 XML 去标签：<...> → 空格
            bool insideTag = false;
            for (char ch : std::string(raw.begin(), raw.end())) {
                if (ch == '<') { insideTag = true; continue; }
                if (ch == '>') { insideTag = false; text.push_back(' '); continue; }
                if (!insideTag) text.push_back(ch);
            }
            text.push_back('\n');
        }
    } catch (const SecurityError &) {
        return text;
    }
    return text;
}

Report scanPath(const std::filesystem::path &path, bool recursive)
{
    Report report;
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        report.results.push_back(FileResult{path, 0, {}, true, "路径不存在"});
        return report;
    }

    const auto scanOne = [&report](const std::filesystem::path &file) {
        if (isReparsePoint(file)) {
            ++report.filesSkipped;
            report.results.push_back(FileResult{file, 0, {}, true, "重解析点（链接/目录联结），跳过"});
            return;
        }
        if (isCloudPlaceholder(file)) {
            ++report.filesSkipped;
            report.results.push_back(FileResult{file, 0, {}, true, "云盘占位文件（仅在线），跳过正文读取"});
            return;
        }
        std::size_t bytes = 0;
        std::string reason;
        const std::string text = readFileText(file, &bytes, &reason);
        if (!reason.empty() && text.empty()) {
            ++report.filesSkipped;
            report.results.push_back(FileResult{file, bytes, {}, true, reason});
            return;
        }
        FileResult fileResult;
        fileResult.path = file;
        fileResult.bytes = bytes;
        fileResult.findings = scanText(text, pathUtf8(file.filename()));
        ++report.filesScanned;
        for (const Finding &finding : fileResult.findings) {
            if (finding.severity == Severity::High)
                ++report.high;
            else if (finding.severity == Severity::Medium)
                ++report.medium;
            else if (finding.severity == Severity::Low)
                ++report.low;
        }
        report.results.push_back(std::move(fileResult));
    };

    if (std::filesystem::is_regular_file(path, error)) {
        scanOne(path);
        return report;
    }

    if (recursive) {
        std::filesystem::recursive_directory_iterator it(
            path, std::filesystem::directory_options::skip_permission_denied, error);
        for (const auto end = std::filesystem::recursive_directory_iterator(); it != end; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (isReparsePoint(it->path())) {
                if (it->is_directory(error))
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(error))
                scanOne(it->path());
        }
    } else {
        for (const auto &entry : std::filesystem::directory_iterator(
                 path, std::filesystem::directory_options::skip_permission_denied, error)) {
            if (entry.is_regular_file(error))
                scanOne(entry.path());
        }
    }
    return report;
}

} // namespace file_guard::scan
