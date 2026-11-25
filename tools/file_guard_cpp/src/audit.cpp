#define _CRT_SECURE_NO_WARNINGS
#include "file_guard/audit.h"

#include "file_guard/crypto.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace file_guard::audit {

namespace {

using crypto::Bytes;

constexpr const char *kGenesisLabel = "FG-AUDIT-GENESIS-v1";
constexpr const char *kChainLabel = "FG-AUDIT-CHAIN-v2";
constexpr const char *kMetaLabel = "FG-AUDIT-META-v2";
constexpr const char *kAadLabel = "FGL1";
constexpr const char *kMetaFile = "audit.meta.json";
constexpr const char *kLogFile = "audit.jsonl";

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

Bytes hexToBytes(const std::string &hex)
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

std::string bytesToHex(const Bytes &data)
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

std::string be64(std::uint64_t value)
{
    Bytes out;
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    return std::string(reinterpret_cast<const char *>(out.data()), out.size());
}

std::string escapeJson(const std::string &text)
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

std::string unescapeJson(const std::string &text)
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

std::string lastNonEmptyLine(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end <= 0)
        return {};
    const std::size_t size = static_cast<std::size_t>(end);
    const std::size_t window = std::min<std::size_t>(size, 1u << 20);
    in.seekg(static_cast<std::streamoff>(size - window), std::ios::beg);
    std::string chunk(window, '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(window));
    chunk.resize(static_cast<std::size_t>(in.gcount()));
    const std::size_t pos = chunk.find_last_of('\n');
    if (pos == std::string::npos)
        return {};
    const std::size_t prev = chunk.find_last_of('\n', pos == 0 ? 0 : pos - 1);
    const std::size_t begin = (prev == std::string::npos) ? 0 : prev + 1;
    return chunk.substr(begin, pos - begin);
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

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

void writeFileAtomic(const std::filesystem::path &path, const std::string &data)
{
    const std::filesystem::path tmp = path.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw SecurityError(ErrorCode::Io, "无法写入审计文件");
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out)
            throw SecurityError(ErrorCode::Io, "写入审计文件失败（磁盘满？）");
    }
    if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw SecurityError(ErrorCode::Io, "替换审计文件失败");
}

std::string jsonString(const std::string &line, const std::string &key)
{
    const std::string needle = "\"" + key + "\":\"";
    std::size_t pos = line.find(needle);
    if (pos == std::string::npos)
        return {};
    pos += needle.size();
    std::string value;
    for (; pos < line.size(); ++pos) {
        if (line[pos] == '\\' && pos + 1 < line.size()) {
            value.push_back(line[pos]);
            value.push_back(line[++pos]);
            continue;
        }
        if (line[pos] == '"')
            break;
        value.push_back(line[pos]);
    }
    return unescapeJson(value);
}

std::uint64_t jsonUint(const std::string &line, const std::string &key, bool *found = nullptr)
{
    const std::string needle = "\"" + key + "\":";
    std::size_t pos = line.find(needle);
    if (found)
        *found = pos != std::string::npos;
    if (pos == std::string::npos)
        return 0;
    pos += needle.size();
    std::uint64_t value = 0;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(line[pos] - '0');
        ++pos;
    }
    return value;
}

struct Meta {
    Bytes salt;
    std::uint32_t iterations = 100'000;
    std::uint64_t nextSeq = 0;
    std::string head;
    std::string mac;
    std::string raw;
    bool exists = false;
};

class Keys {
public:
    explicit Keys(const Bytes &key) : key(key) {}
    Bytes chain() const { return crypto::hmacSha256(key, Bytes(kChainLabel, kChainLabel + std::strlen(kChainLabel))); }
    Bytes meta() const { return crypto::hmacSha256(key, Bytes(kMetaLabel, kMetaLabel + std::strlen(kMetaLabel))); }
    Bytes key;
};

std::string chainHash(const Bytes &chainKey, const std::string &prevHeadHex,
                      const Bytes &nonce, const Bytes &cipher)
{
    Bytes data = hexToBytes(prevHeadHex);
    data.insert(data.end(), nonce.begin(), nonce.end());
    data.insert(data.end(), cipher.begin(), cipher.end());
    return bytesToHex(crypto::hmacSha256(chainKey, data));
}

Bytes metaBody(const Meta &meta)
{
    std::string body = "2|" + base64Encode(meta.salt) + "|" +
                       std::to_string(meta.iterations) + "|" +
                       std::to_string(meta.nextSeq) + "|" + meta.head;
    return Bytes(body.begin(), body.end());
}

std::string genesisHead(const Bytes &salt)
{
    Bytes data(kGenesisLabel, kGenesisLabel + std::strlen(kGenesisLabel));
    data.insert(data.end(), salt.begin(), salt.end());
    return bytesToHex(crypto::sha256(data));
}

Bytes recordAad(std::uint64_t seq, const Bytes &nonce)
{
    Bytes aad(kAadLabel, kAadLabel + std::strlen(kAadLabel));
    const std::string seqBytes = be64(seq);
    aad.insert(aad.end(), seqBytes.begin(), seqBytes.end());
    aad.insert(aad.end(), nonce.begin(), nonce.end());
    return aad;
}

} // namespace

AuditLog::AuditLog(std::filesystem::path directory, std::string password,
                   std::uint32_t iterations)
    : m_directory(std::move(directory))
    , m_password(std::move(password))
    , m_iterations(iterations)
{
    if (m_password.empty())
        throw SecurityError(ErrorCode::Password, "审计密码不能为空");
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (error)
        throw SecurityError(ErrorCode::Io, "无法创建审计目录");
}

std::string AuditLog::head() const
{
    const std::string raw = readFile(m_directory / kMetaFile);
    if (raw.empty())
        return {};
    Meta meta;
    meta.salt = base64Decode(jsonString(raw, "salt"));
    meta.iterations = static_cast<std::uint32_t>(jsonUint(raw, "iterations"));
    meta.nextSeq = jsonUint(raw, "next_seq");
    meta.head = jsonString(raw, "head");
    meta.mac = jsonString(raw, "mac");
    const Bytes key = crypto::pbkdf2(m_password, meta.salt, meta.iterations);
    const Bytes mac = crypto::hmacSha256(Keys(key).meta(), metaBody(meta));
    if (bytesToHex(mac) != meta.mac)
        throw SecurityError(ErrorCode::Integrity, "审计 meta 签名校验失败（可能被篡改）");
    return meta.head;
}

std::uint64_t AuditLog::nextSeq() const
{
    const std::string raw = readFile(m_directory / kMetaFile);
    return jsonUint(raw, "next_seq");
}

std::uint64_t AuditLog::append(const std::string &event, const std::string &actor,
                               const std::string &detail)
{
    Meta meta;
    meta.raw = readFile(m_directory / kMetaFile);
    if (meta.raw.empty()) {
        meta.salt = crypto::randomBytes(16);
        meta.iterations = m_iterations;
        meta.nextSeq = 0;
        meta.head = genesisHead(meta.salt);
        meta.exists = true;
    } else {
        meta.salt = base64Decode(jsonString(meta.raw, "salt"));
        meta.iterations = static_cast<std::uint32_t>(jsonUint(meta.raw, "iterations"));
        meta.nextSeq = jsonUint(meta.raw, "next_seq");
        meta.head = jsonString(meta.raw, "head");
        meta.exists = true;
        if (meta.salt.size() != 16 || meta.iterations < 1000)
            throw SecurityError(ErrorCode::Corrupt, "审计 meta 结构损坏");
    }

    const Bytes key = crypto::pbkdf2(m_password, meta.salt, meta.iterations);
    const Keys keys(key);
    const Bytes expectedMac = crypto::hmacSha256(keys.meta(), metaBody(meta));

    // 追加前一致性预检：日志尾部必须与 meta 高水位/链头一致，否则拒绝写入
    // （崩溃导致“日志已写、meta 未更新”时会在这里暴露，而不是悄悄写坏链）。
    if (meta.nextSeq > 0 && meta.raw.find("\"mac\"") != std::string::npos) {
        const std::string last = lastNonEmptyLine(m_directory / kLogFile);
        if (last.empty())
            throw SecurityError(ErrorCode::Integrity,
                                "审计日志缺失但 meta 有高水位（可能被删除/回滚）");
        const std::uint64_t lastSeq = jsonUint(last, "seq");
        if (lastSeq + 1 != meta.nextSeq)
            throw SecurityError(ErrorCode::Integrity,
                                "审计日志尾部序号与 meta 不一致（可能被截断/回滚），请先恢复或清理");
        if (jsonString(last, "h") != meta.head)
            throw SecurityError(ErrorCode::Integrity,
                                "审计日志尾部链哈希与 meta 不一致（尾部被替换？）");
    }

    if (!jsonString(meta.raw, "mac").empty() && bytesToHex(expectedMac) != jsonString(meta.raw, "mac"))
        throw SecurityError(ErrorCode::Integrity, "审计 meta 签名校验失败（可能被篡改或被回滚）");

    const std::uint64_t seq = meta.nextSeq;
    const Bytes nonce = crypto::randomBytes(12);
    const std::string payload = "{\"event\":\"" + escapeJson(event) +
                                "\",\"actor\":\"" + escapeJson(actor) +
                                "\",\"detail\":\"" + escapeJson(detail) +
                                "\",\"ts\":\"" + nowIso() + "\"}";
    const Bytes plain(payload.begin(), payload.end());
    const Bytes cipher = crypto::aeadEncrypt(key, nonce, recordAad(seq, nonce), plain);
    const std::string chain = chainHash(keys.chain(), seq == 0 ? genesisHead(meta.salt) : meta.head,
                                        nonce, cipher);

    std::ofstream log(m_directory / kLogFile, std::ios::binary | std::ios::app);
    if (!log)
        throw SecurityError(ErrorCode::Io, "无法追加审计日志");
    log << "{\"seq\":" << seq << ",\"n\":\"" << base64Encode(nonce) << "\",\"h\":\""
        << chain << "\",\"d\":\"" << base64Encode(cipher) << "\"}\n";
    log.flush();
    if (!log)
        throw SecurityError(ErrorCode::Io, "写入审计日志失败（磁盘满？）");

    meta.nextSeq = seq + 1;
    meta.head = chain;
    const Bytes mac = crypto::hmacSha256(keys.meta(), metaBody(meta));
    const std::string metaJson =
        "{\"version\":2,\"salt\":\"" + base64Encode(meta.salt) + "\",\"iterations\":" +
        std::to_string(meta.iterations) + ",\"next_seq\":" + std::to_string(meta.nextSeq) +
        ",\"head\":\"" + meta.head + "\",\"mac\":\"" + bytesToHex(mac) + "\"}";
    writeFileAtomic(m_directory / kMetaFile, metaJson);
    return seq;
}

VerifyResult AuditLog::verify() const
{
    VerifyResult result;
    const std::string raw = readFile(m_directory / kMetaFile);
    if (raw.empty()) {
        result.ok = true; // 空日志：视为合法起点
        return result;
    }
    Meta meta;
    meta.salt = base64Decode(jsonString(raw, "salt"));
    meta.iterations = static_cast<std::uint32_t>(jsonUint(raw, "iterations"));
    meta.nextSeq = jsonUint(raw, "next_seq");
    meta.head = jsonString(raw, "head");
    if (meta.salt.size() != 16 || meta.iterations < 1000) {
        result.firstProblem = "审计 meta 结构损坏";
        return result;
    }
    const Bytes key = crypto::pbkdf2(m_password, meta.salt, meta.iterations);
    const Keys keys(key);
    const std::string storedMac = jsonString(raw, "mac");
    if (storedMac.empty() ||
        bytesToHex(crypto::hmacSha256(keys.meta(), metaBody(meta))) != storedMac) {
        result.firstProblem = "审计 meta 签名校验失败（可能被篡改）";
        return result;
    }

    std::ifstream log(m_directory / kLogFile, std::ios::binary);
    std::string line;
    std::string prev = genesisHead(meta.salt);
    std::uint64_t expectedSeq = 0;
    while (std::getline(log, line)) {
        if (line.empty())
            continue;
        const std::uint64_t seq = jsonUint(line, "seq");
        const std::string nonceB64 = jsonString(line, "n");
        const std::string storedChain = jsonString(line, "h");
        const std::string cipherB64 = jsonString(line, "d");
        if (seq != expectedSeq || nonceB64.empty() || storedChain.empty() || cipherB64.empty()) {
            ++result.problems;
            if (result.firstProblem.empty())
                result.firstProblem = "第 " + std::to_string(expectedSeq) + " 条记录结构非法或序号不连续";
            break;
        }
        const Bytes nonce = base64Decode(nonceB64);
        const Bytes cipher = base64Decode(cipherB64);
        if (nonce.size() != 12 || cipher.size() < 16) {
            ++result.problems;
            if (result.firstProblem.empty())
                result.firstProblem = "第 " + std::to_string(seq) + " 条记录密文结构非法";
            break;
        }
        if (chainHash(keys.chain(), prev, nonce, cipher) != storedChain) {
            ++result.problems;
            if (result.firstProblem.empty())
                result.firstProblem = "第 " + std::to_string(seq) + " 条哈希校验失败（被修改/删除/重排）";
            break;
        }
        try {
            crypto::aeadDecrypt(key, nonce, recordAad(seq, nonce), cipher);
        } catch (const SecurityError &) {
            ++result.problems;
            if (result.firstProblem.empty())
                result.firstProblem = "第 " + std::to_string(seq) + " 条解密失败";
            break;
        }
        prev = storedChain;
        ++expectedSeq;
    }

    result.records = expectedSeq;
    if (expectedSeq < meta.nextSeq) {
        ++result.problems;
        if (result.firstProblem.empty())
            result.firstProblem = "日志被截断/回滚（高水位 " + std::to_string(meta.nextSeq) +
                                  "，实际 " + std::to_string(expectedSeq) + "）";
    }
    if (prev != meta.head) {
        ++result.problems;
        if (result.firstProblem.empty())
            result.firstProblem = "链头与 meta 不一致（尾部被截断或替换）";
    }
    result.ok = result.problems == 0;
    result.head = prev;
    return result;
}

std::vector<Record> AuditLog::tail(std::size_t limit) const
{
    const std::string raw = readFile(m_directory / kMetaFile);
    if (raw.empty())
        return {};
    Meta meta;
    meta.salt = base64Decode(jsonString(raw, "salt"));
    meta.iterations = static_cast<std::uint32_t>(jsonUint(raw, "iterations"));
    const Bytes key = crypto::pbkdf2(m_password, meta.salt, meta.iterations);

    std::ifstream log(m_directory / kLogFile, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(log, line)) {
        if (!line.empty())
            lines.push_back(line);
    }
    std::vector<Record> records;
    const std::size_t begin = lines.size() > limit ? lines.size() - limit : 0;
    for (std::size_t i = begin; i < lines.size(); ++i) {
        const std::uint64_t seq = jsonUint(lines[i], "seq");
        const Bytes nonce = base64Decode(jsonString(lines[i], "n"));
        const Bytes cipher = base64Decode(jsonString(lines[i], "d"));
        const Bytes plain = crypto::aeadDecrypt(key, nonce, recordAad(seq, nonce), cipher);
        const std::string payload(plain.begin(), plain.end());
        Record record;
        record.seq = seq;
        record.event = jsonString(payload, "event");
        record.actor = jsonString(payload, "actor");
        record.detail = jsonString(payload, "detail");
        record.timestamp = jsonString(payload, "ts");
        records.push_back(std::move(record));
    }
    return records;
}

} // namespace file_guard::audit
