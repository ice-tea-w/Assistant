#include "file_guard/vault.h"

#include "file_guard/audit.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace file_guard {

namespace {

using crypto::Bytes;

constexpr const char *kKeysFile = "vault.keys";
constexpr const char *kContainersDir = "containers";
constexpr const char *kManifestDir = "manifest";
constexpr const char *kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr const char *kAadPassword = "FG-VAULT-DEK-PASSWORD-v1";
constexpr const char *kAadRecovery = "FG-VAULT-DEK-RECOVERY-v1";
constexpr const char *kAadCode = "FG-VAULT-RECOVERY-CODE-v1";
constexpr std::size_t kDekSize = 32;
constexpr std::size_t kNonce = 12;

Bytes aad(const char *label)
{
    return Bytes(label, label + std::strlen(label));
}

std::string wrap(const Bytes &secret, const Bytes &key, const char *label)
{
    const Bytes nonce = crypto::randomBytes(kNonce);
    const Bytes cipher = crypto::aeadEncrypt(key, nonce, aad(label), secret);
    Bytes blob = nonce;
    blob.insert(blob.end(), cipher.begin(), cipher.end());
    return util::base64Encode(blob);
}

Bytes unwrap(const std::string &text, const Bytes &key, const char *label)
{
    const Bytes blob = util::base64Decode(text);
    if (blob.size() <= kNonce)
        throw SecurityError(ErrorCode::Integrity, "保险箱密钥块结构损坏");
    const Bytes nonce(blob.begin(), blob.begin() + kNonce);
    const Bytes cipher(blob.begin() + kNonce, blob.end());
    return crypto::aeadDecrypt(key, nonce, aad(label), cipher);
}

void setMtime(const std::filesystem::path &path, double seconds)
{
    if (seconds <= 0)
        return;
    using namespace std::chrono;
    const auto target = system_clock::from_time_t(static_cast<std::time_t>(seconds));
    const auto delta = target - system_clock::now();
    std::filesystem::last_write_time(
        path, std::filesystem::file_time_type::clock::now() +
                  duration_cast<std::filesystem::file_time_type::duration>(delta));
}

std::string hexOf(const Bytes &data) { return util::hexEncode(data); }

std::string recoveryFromBytes(const Bytes &raw)
{
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (std::uint8_t byte : raw) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kAlphabet[(buffer >> bits) & 31]);
        }
    }
    return out;
}
Bytes bytesOf(const std::string &hex) { return util::hexDecode(hex); }

std::size_t decodeIndex(const std::string &raw)
{
    const std::size_t value = std::strtoull(raw.c_str(), nullptr, 10);
    return static_cast<std::size_t>(value);
}

std::string jsonString(const std::string &line, const std::string &key)
{
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos)
        return {};
    std::string value;
    for (std::size_t i = pos + needle.size(); i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            value.push_back(line[i]);
            value.push_back(line[++i]);
            continue;
        }
        if (line[i] == '"')
            break;
        value.push_back(line[i]);
    }
    return util::jsonUnescape(value);
}

std::uint64_t jsonUint(const std::string &line, const std::string &key)
{
    const std::string needle = "\"" + key + "\":";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos)
        return 0;
    return std::strtoull(line.c_str() + pos + needle.size(), nullptr, 10);
}

double jsonDouble(const std::string &line, const std::string &key)
{
    const std::string needle = "\"" + key + "\":";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos)
        return 0;
    return std::strtod(line.c_str() + pos + needle.size(), nullptr);
}

} // namespace

struct Vault::Keys {
    std::uint32_t version = 1;
    std::uint32_t iterations = 0;
    std::string salt;
    std::string wrappedDek;
    std::uint32_t recoveryIterations = 0;
    std::string recoverySalt;
    std::string wrappedRecoveryDek;
    std::string recoveryCodeUnderDek;
    std::string dpapiDek;
};

Vault::Vault(std::filesystem::path directory, std::uint32_t iterations,
             std::uint32_t logIterations)
    : m_directory(std::move(directory))
    , m_iterations(iterations)
    , m_logIterations(logIterations)
{
    std::error_code ignored;
    std::filesystem::create_directories(m_directory, ignored);
}

bool Vault::exists() const
{
    return std::filesystem::is_regular_file(m_directory / kKeysFile);
}

Vault::Keys Vault::loadKeys() const
{
    const std::string text = util::readText(m_directory / kKeysFile);
    if (text.empty())
        throw SecurityError(ErrorCode::InvalidArgument, "保险箱不存在：" + util::pathUtf8(m_directory));
    Keys keys;
    for (const std::string &line : util::splitLines(text)) {
        const std::string trimmed = util::trim(line);
        const std::size_t equal = trimmed.find('=');
        if (equal == std::string::npos)
            continue;
        const std::string key = util::trim(trimmed.substr(0, equal));
        const std::string value = util::trim(trimmed.substr(equal + 1));
        if (key == "version")
            keys.version = static_cast<std::uint32_t>(decodeIndex(value));
        else if (key == "iterations")
            keys.iterations = static_cast<std::uint32_t>(decodeIndex(value));
        else if (key == "salt")
            keys.salt = value;
        else if (key == "wrapped_dek")
            keys.wrappedDek = value;
        else if (key == "recovery_iterations")
            keys.recoveryIterations = static_cast<std::uint32_t>(decodeIndex(value));
        else if (key == "recovery_salt")
            keys.recoverySalt = value;
        else if (key == "wrapped_recovery_dek")
            keys.wrappedRecoveryDek = value;
        else if (key == "recovery_code_under_dek")
            keys.recoveryCodeUnderDek = value;
        else if (key == "dpapi_dek")
            keys.dpapiDek = value;
    }
    if (keys.salt.empty() || keys.wrappedDek.empty())
        throw SecurityError(ErrorCode::Corrupt, "保险箱密钥文件损坏");
    return keys;
}

void Vault::saveKeys(const Keys &keys) const
{
    std::string text = "# file_guard C++ vault keys（勿手改）\n";
    text += "version=" + std::to_string(keys.version) + "\n";
    text += "iterations=" + std::to_string(keys.iterations) + "\n";
    text += "salt=" + keys.salt + "\n";
    text += "wrapped_dek=" + keys.wrappedDek + "\n";
    text += "recovery_iterations=" + std::to_string(keys.recoveryIterations) + "\n";
    text += "recovery_salt=" + keys.recoverySalt + "\n";
    text += "wrapped_recovery_dek=" + keys.wrappedRecoveryDek + "\n";
    text += "recovery_code_under_dek=" + keys.recoveryCodeUnderDek + "\n";
    text += "dpapi_dek=" + keys.dpapiDek + "\n";
    util::writeTextAtomic(m_directory / kKeysFile, text);
    util::restrictAcl(m_directory / kKeysFile);
}

Vault::CreateResult Vault::create(const std::string &password, bool useDpapi)
{
    if (exists())
        throw SecurityError(ErrorCode::InvalidArgument, "保险箱已存在");
    if (password.empty())
        throw SecurityError(ErrorCode::Password, "口令不能为空");

    const Bytes dek = crypto::randomBytes(kDekSize);
    const Bytes passwordSalt = crypto::randomBytes(16);
    const Bytes recoverySalt = crypto::randomBytes(16);
    const Bytes codeBytes = crypto::randomBytes(20);

    const Bytes passwordKek =
        crypto::pbkdf2(password, passwordSalt, m_iterations);
    const Bytes recoveryKek =
        crypto::pbkdf2(hexOf(codeBytes), recoverySalt, m_iterations);

    Keys keys;
    keys.iterations = m_iterations;
    keys.salt = util::base64Encode(passwordSalt);
    keys.wrappedDek = wrap(dek, passwordKek, kAadPassword);
    keys.recoveryIterations = m_iterations;
    keys.recoverySalt = util::base64Encode(recoverySalt);
    keys.wrappedRecoveryDek = wrap(dek, recoveryKek, kAadRecovery);
    keys.recoveryCodeUnderDek = wrap(codeBytes, dek, kAadCode);
    if (useDpapi)
        keys.dpapiDek = util::base64Encode(util::dpapiProtect(dek));

    saveKeys(keys);
    m_dek = hexOf(dek);

    std::error_code ignored;
    std::filesystem::create_directories(m_directory / kContainersDir, ignored);

    CreateResult result;
    result.recoveryCode = formatRecoveryCode(recoveryFromBytes(codeBytes));
    result.dpapi = !keys.dpapiDek.empty();
    return result;
}

std::string Vault::normalizeRecoveryCode(const std::string &text)
{
    std::string cleaned;
    for (char ch : text) {
        if (!std::isalnum(static_cast<unsigned char>(ch)))
            continue;
        char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (upper == 'I' || upper == 'L')
            upper = '1';
        else if (upper == 'O')
            upper = '0';
        else if (upper == 'U')
            upper = 'V';
        cleaned.push_back(upper);
    }
    if (cleaned.size() != 32)
        throw SecurityError(ErrorCode::Password, "恢复码长度不对（应为 32 个字符）");
    for (char ch : cleaned) {
        if (std::strchr(kAlphabet, ch) == nullptr)
            throw SecurityError(ErrorCode::Password, "恢复码包含非法字符");
    }
    return cleaned;
}

std::string Vault::formatRecoveryCode(const std::string &normalized)
{
    std::string out;
    for (std::size_t i = 0; i < normalized.size(); i += 4) {
        if (!out.empty())
            out.push_back('-');
        out += normalized.substr(i, 4);
    }
    return out;
}

void Vault::unlock(const std::string &password)
{
    const Keys keys = loadKeys();
    const Bytes kek = crypto::pbkdf2(password, util::base64Decode(keys.salt),
                                     keys.iterations);
    m_dek = hexOf(unwrap(keys.wrappedDek, kek, kAadPassword));
}

void Vault::unlockWithRecovery(const std::string &recoveryCode)
{
    const Keys keys = loadKeys();
    const std::string normalized = normalizeRecoveryCode(recoveryCode);
    // 32 个 Crockford 字符 = 20 字节 = 160 bit
    Bytes codeBytes;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char ch : normalized) {
        const char *found = std::strchr(kAlphabet, ch);
        if (!found)
            throw SecurityError(ErrorCode::Password, "恢复码包含非法字符");
        buffer = (buffer << 5) | static_cast<std::uint32_t>(found - kAlphabet);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            codeBytes.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    const Bytes kek = crypto::pbkdf2(hexOf(codeBytes),
                                     util::base64Decode(keys.recoverySalt),
                                     keys.recoveryIterations);
    m_dek = hexOf(unwrap(keys.wrappedRecoveryDek, kek, kAadRecovery));
}

void Vault::unlockWithDpapi()
{
    const Keys keys = loadKeys();
    if (keys.dpapiDek.empty())
        throw SecurityError(ErrorCode::Password, "该保险箱未启用 DPAPI 免密");
    m_dek = hexOf(util::dpapiUnprotect(util::base64Decode(keys.dpapiDek)));
}

void Vault::lock() { m_dek.clear(); }

std::string Vault::requireDek() const
{
    if (m_dek.empty())
        throw SecurityError(ErrorCode::Password, "保险箱未解锁：请先输入口令或恢复码");
    return m_dek;
}

std::string Vault::manifestPassword() const
{
    return "fg-vault-manifest:" + requireDek();
}

std::vector<std::string> Vault::manifestRecords()
{
    audit::AuditLog log(m_directory / kManifestDir, manifestPassword(), m_logIterations);
    std::vector<std::string> out;
    for (const auto &record : log.tail(10'000'000))
        out.push_back(record.detail);
    return out;
}

std::filesystem::path Vault::safeContainer(const std::string &relative) const
{
    if (relative.rfind("containers/", 0) != 0)
        throw SecurityError(ErrorCode::Integrity, "清单里的容器路径非法");
    const std::filesystem::path path = (m_directory / relative).lexically_normal();
    const std::filesystem::path root = (m_directory / kContainersDir).lexically_normal();
    if (util::pathUtf8(path).rfind(util::pathUtf8(root), 0) != 0)
        throw SecurityError(ErrorCode::Integrity, "容器路径越界");
    return path;
}

std::vector<VaultFileInfo> Vault::list()
{
    requireDek();
    std::vector<VaultFileInfo> files;
    for (const std::string &detail : manifestRecords()) {
        VaultFileInfo info;
        info.name = jsonString(detail, "name");
        info.container = jsonString(detail, "container");
        info.size = jsonUint(detail, "size");
        info.sha256 = jsonString(detail, "sha256");
        info.ts = jsonString(detail, "ts");
        info.note = jsonString(detail, "note");
        info.mtime = jsonDouble(detail, "mtime");
        if (!info.container.empty())
            files.push_back(std::move(info));
    }
    return files;
}

VaultFileInfo Vault::addFile(const std::filesystem::path &source, const std::string &note,
                             bool removeSource)
{
    const std::string dek = requireDek();
    if (!std::filesystem::is_regular_file(source))
        throw SecurityError(ErrorCode::Io, "文件不存在：" + util::pathUtf8(source));
    std::error_code ignored;
    std::filesystem::create_directories(m_directory / kContainersDir, ignored);

    const std::string containerName = util::randomHex(16) + ".fgc";
    const std::filesystem::path container = m_directory / kContainersDir / containerName;

    const crypto::Result result = crypto::encryptFile(source, container, "fg-vault:" + dek,
                                                      m_iterations, crypto::kDefaultChunkSize);

    audit::AuditLog log(m_directory / kManifestDir, manifestPassword(), m_logIterations);
    const std::string detail =
        "{\"kind\":\"file\",\"container\":\"containers/" + containerName +
        "\",\"name\":\"" + util::jsonEscape(util::pathUtf8(source.filename())) +
        "\",\"size\":" + std::to_string(result.plaintextSize) + ",\"sha256\":\"" +
        util::hexEncode(result.sha256) + "\",\"ts\":\"" + util::nowIso() + "\",\"note\":\"" +
        util::jsonEscape(note) + "\",\"mtime\":" +
        std::to_string(std::filesystem::last_write_time(source).time_since_epoch().count() / 10000000.0) + "}";
    log.append("vault.add", "vault", detail);

    if (removeSource) {
        const crypto::Result check = crypto::verifyFile(container, "fg-vault:" + dek);
        if (!crypto::constantTimeEquals(check.sha256, result.sha256)) {
            std::filesystem::remove(container, ignored);
            throw SecurityError(ErrorCode::Integrity, "加密结果校验失败，已保留源文件");
        }
        std::filesystem::remove(source, ignored);
    }

    VaultFileInfo info;
    info.name = util::pathUtf8(source.filename());
    info.container = "containers/" + containerName;
    info.size = result.plaintextSize;
    info.sha256 = util::hexEncode(result.sha256);
    info.ts = util::nowIso();
    info.note = note;
    info.mtime = std::filesystem::last_write_time(source).time_since_epoch().count() / 10000000.0;
    return info;
}

Vault::ExtractResult Vault::extract(const std::string &target, std::filesystem::path destDir,
                                    bool overwrite)
{
    requireDek();
    const std::vector<VaultFileInfo> files = list();
    if (files.empty())
        throw SecurityError(ErrorCode::InvalidArgument, "保险箱里没有文件");

    const VaultFileInfo *matched = nullptr;
    if (!target.empty() && std::isdigit(static_cast<unsigned char>(target[0]))) {
        const std::size_t index = decodeIndex(target);
        if (index < 1 || index > files.size())
            throw SecurityError(ErrorCode::InvalidArgument, "序号超出范围");
        matched = &files[index - 1];
    } else {
        for (const auto &file : files) {
            const std::string containerName = file.container.substr(file.container.rfind('/') + 1);
            if (file.name == target || file.container == target || containerName == target) {
                if (matched)
                    throw SecurityError(ErrorCode::InvalidArgument, "匹配到多个文件，请用序号指定");
                matched = &file;
            }
        }
    }
    if (!matched)
        throw SecurityError(ErrorCode::InvalidArgument, "保险箱里没有匹配的文件：" + target);

    const std::filesystem::path container = safeContainer(matched->container);
    const std::filesystem::path outDir = destDir.empty() ? m_directory / "restored" : destDir;
    std::error_code ignored;
    std::filesystem::create_directories(outDir, ignored);
    const std::filesystem::path dest = outDir / util::fromUtf8(matched->name);
    if (std::filesystem::exists(dest) && !overwrite)
        throw SecurityError(ErrorCode::InvalidArgument, "目标已存在：" + util::pathUtf8(dest));
    if (std::filesystem::exists(dest))
        std::filesystem::remove(dest, ignored);

    const crypto::Result result = crypto::decryptFile(container, dest, "fg-vault:" + requireDek());
    if (!crypto::constantTimeEquals(result.sha256, bytesOf(matched->sha256))) {
        std::filesystem::remove(dest, ignored);
        throw SecurityError(ErrorCode::Integrity, "还原后哈希与清单不一致");
    }

    setMtime(dest, matched->mtime);
    ExtractResult out;
    out.name = matched->name;
    out.restoredTo = util::pathUtf8(dest);
    out.sha256 = matched->sha256;
    out.size = matched->size;
    return out;
}

void Vault::changePassword(const std::string &oldPassword, const std::string &newPassword)
{
    const Keys keys = loadKeys();
    const Bytes oldKek = crypto::pbkdf2(oldPassword, util::base64Decode(keys.salt),
                                        keys.iterations);
    const Bytes dek = unwrap(keys.wrappedDek, oldKek, kAadPassword);

    const Bytes newSalt = crypto::randomBytes(16);
    const Bytes newKek = crypto::pbkdf2(newPassword, newSalt, m_iterations);
    Keys updated = keys;
    updated.iterations = m_iterations;
    updated.salt = util::base64Encode(newSalt);
    updated.wrappedDek = wrap(dek, newKek, kAadPassword);
    if (!updated.dpapiDek.empty())
        updated.dpapiDek = util::base64Encode(util::dpapiProtect(dek));
    saveKeys(updated);
    m_dek = hexOf(dek);
}

std::string Vault::recoveryCode(const std::string &password)
{
    const Keys keys = loadKeys();
    const Bytes kek = crypto::pbkdf2(password, util::base64Decode(keys.salt),
                                     keys.iterations);
    const Bytes dek = unwrap(keys.wrappedDek, kek, kAadPassword);
    const Bytes codeBytes = unwrap(keys.recoveryCodeUnderDek, dek, kAadCode);
    return formatRecoveryCode(recoveryFromBytes(codeBytes));
}

Vault::VerifyResult Vault::verify()
{
    requireDek();
    VerifyResult result;
    try {
        audit::AuditLog log(m_directory / kManifestDir, manifestPassword(), m_logIterations);
        const auto verify = log.verify();
        if (!verify.ok) {
            result.problems = verify.problems;
            result.firstProblem = verify.firstProblem;
            return result;
        }
        for (const VaultFileInfo &info : list()) {
            ++result.files;
            try {
                const crypto::Result check =
                    crypto::verifyFile(safeContainer(info.container), "fg-vault:" + requireDek());
                if (!crypto::constantTimeEquals(check.sha256, bytesOf(info.sha256))) {
                    ++result.problems;
                    if (result.firstProblem.empty())
                        result.firstProblem = info.name + "：内容与清单不一致";
                }
            } catch (const SecurityError &error) {
                ++result.problems;
                if (result.firstProblem.empty())
                    result.firstProblem = info.name + "：" + error.what();
            }
        }
    } catch (const SecurityError &error) {
        ++result.problems;
        result.firstProblem = error.what();
    }
    result.ok = result.problems == 0;
    return result;
}

} // namespace file_guard
