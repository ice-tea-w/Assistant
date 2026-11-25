#include "file_guard/integrity.h"

#include "file_guard/crypto.h"
#include "file_guard/util.h"

#include <algorithm>
#include <sstream>

namespace file_guard {

namespace {

constexpr const char *kLabel = "FG-INTEGRITY-v1";
constexpr const char *kMacPrefix = "mac=";

crypto::Bytes integrityKey(const std::string &password, std::uint32_t iterations)
{
    crypto::Bytes salt(16, 0x2A);
    return crypto::pbkdf2(password, salt, iterations);
}

} // namespace

IntegrityReport buildIntegrity(const std::vector<std::filesystem::path> &files,
                               const std::filesystem::path &manifest,
                               const std::string &password, std::uint32_t iterations)
{
    IntegrityReport report;
    std::ostringstream body;
    for (const auto &file : files) {
        if (!std::filesystem::is_regular_file(file)) {
            ++report.problems;
            if (report.firstProblem.empty())
                report.firstProblem = "文件不存在：" + util::pathUtf8(file);
            continue;
        }
        const crypto::Bytes digest = crypto::sha256File(file);
        body << util::hexEncode(digest) << "  " << util::pathUtf8(file) << "\n";
        ++report.files;
    }
    const std::string bodyText = body.str();
    const crypto::Bytes key = integrityKey(password, iterations);
    const crypto::Bytes mac = crypto::hmacSha256(key, crypto::Bytes(bodyText.begin(), bodyText.end()));
    util::writeTextAtomic(manifest, bodyText + kMacPrefix + util::hexEncode(mac) + "\n");
    report.ok = report.problems == 0;
    return report;
}

IntegrityReport verifyIntegrity(const std::filesystem::path &manifest,
                                const std::string &password, std::uint32_t iterations)
{
    IntegrityReport report;
    const std::string text = util::readText(manifest);
    if (text.empty()) {
        ++report.problems;
        report.firstProblem = "清单不存在或为空";
        return report;
    }

    std::string body;
    std::string storedMac;
    for (const std::string &line : util::splitLines(text)) {
        if (line.rfind(kMacPrefix, 0) == 0)
            storedMac = line.substr(std::strlen(kMacPrefix));
        else if (!line.empty())
            body += line + "\n";
    }
    if (storedMac.empty()) {
        ++report.problems;
        report.firstProblem = "清单缺少 HMAC 签名";
        return report;
    }

    const crypto::Bytes key = integrityKey(password, iterations);
    const crypto::Bytes expected =
        crypto::hmacSha256(key, crypto::Bytes(body.begin(), body.end()));
    if (!crypto::constantTimeEquals(expected, util::hexDecode(storedMac))) {
        ++report.problems;
        report.firstProblem = "清单签名校验失败（清单被修改）";
        return report;
    }

    for (const std::string &line : util::splitLines(body)) {
        const std::string trimmed = util::trim(line);
        if (trimmed.empty())
            continue;
        const std::size_t space = trimmed.find("  ");
        if (space == std::string::npos) {
            ++report.problems;
            if (report.firstProblem.empty())
                report.firstProblem = "清单行格式非法";
            continue;
        }
        const std::string expectedSha = trimmed.substr(0, space);
        const std::string path = trimmed.substr(space + 2);
        ++report.files;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(path))) {
            ++report.problems;
            if (report.firstProblem.empty())
                report.firstProblem = "文件缺失：" + path;
            continue;
        }
        const std::string actual = util::hexEncode(crypto::sha256File(std::filesystem::u8path(path)));
        if (actual != expectedSha) {
            ++report.problems;
            if (report.firstProblem.empty())
                report.firstProblem = "文件被修改：" + path;
        }
    }
    report.ok = report.problems == 0;
    return report;
}

} // namespace file_guard
