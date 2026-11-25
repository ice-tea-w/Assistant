#include "file_guard/backup.h"

#include "file_guard/audit.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace file_guard {

namespace {

using crypto::Bytes;

constexpr const char *kObjectLabel = "FG-OBJ-v1";
constexpr const char *kObjectsDir = "objects";

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

BackupStore::BackupStore(std::filesystem::path directory, std::string password,
                         std::uint32_t iterations, std::uint32_t logIterations)
    : m_directory(std::move(directory))
    , m_password(std::move(password))
    , m_iterations(iterations)
    , m_logIterations(logIterations)
{
    if (m_password.empty())
        throw SecurityError(ErrorCode::Password, "备份密码不能为空");
    std::error_code ignored;
    std::filesystem::create_directories(m_directory, ignored);
    std::filesystem::create_directories(m_directory / kObjectsDir, ignored);
}

std::filesystem::path BackupStore::objectPath(const std::string &object) const
{
    if (object.size() < 2)
        throw SecurityError(ErrorCode::Integrity, "对象名非法");
    return m_directory / kObjectsDir / object.substr(0, 2) / (object + ".fgc");
}

std::string BackupStore::objectPassword() const
{
    return m_password + "|object";
}

std::string BackupStore::objectNameFor(const std::filesystem::path &source) const
{
    const Bytes digest = crypto::sha256File(source);
    const Bytes salt(16, 0x1F);
    const Bytes key = crypto::pbkdf2(m_password, salt, m_iterations);
    return util::hexEncode(crypto::hmacSha256(key, digest));
}

std::vector<BackupEntry> BackupStore::readIndex()
{
    std::vector<BackupEntry> entries;
    try {
        audit::AuditLog log(m_directory / "index", m_password + "|index", m_logIterations);
        for (const auto &record : log.tail(10'000'000)) {
            const std::string &detail = record.detail;
            BackupEntry entry;
            entry.path = jsonString(detail, "path");
            entry.version = static_cast<std::uint32_t>(jsonUint(detail, "version"));
            entry.object = jsonString(detail, "object");
            entry.sha256 = jsonString(detail, "sha256");
            entry.size = jsonUint(detail, "size");
            entry.mtime = jsonDouble(detail, "mtime");
            entry.ts = jsonString(detail, "ts");
            if (!entry.object.empty())
                entries.push_back(std::move(entry));
        }
    } catch (const SecurityError &) {
        // 索引不存在 = 空库
    }
    return entries;
}

std::vector<BackupEntry> BackupStore::list()
{
    return readIndex();
}

std::vector<BackupEntry> BackupStore::versions(const std::string &path)
{
    std::vector<BackupEntry> out;
    for (const auto &entry : readIndex()) {
        if (entry.path == path)
            out.push_back(entry);
    }
    std::sort(out.begin(), out.end(),
              [](const BackupEntry &a, const BackupEntry &b) { return a.version < b.version; });
    return out;
}

BackupEntry BackupStore::backupFile(const std::filesystem::path &source)
{
    if (!std::filesystem::is_regular_file(source))
        throw SecurityError(ErrorCode::Io, "文件不存在：" + util::pathUtf8(source));
    const std::string absolute = util::pathUtf8(std::filesystem::absolute(source));
    const std::string object = objectNameFor(source);
    const std::filesystem::path target = objectPath(object);
    std::error_code ignored;
    std::filesystem::create_directories(target.parent_path(), ignored);

    const crypto::Result result =
        crypto::encryptFile(source, target, objectPassword(), m_iterations,
                            crypto::kDefaultChunkSize);

    std::uint32_t version = 1;
    for (const auto &entry : versions(absolute))
        version = std::max(version, entry.version + 1);

    const double mtime = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::filesystem::last_write_time(source).time_since_epoch())
            .count());

    const std::string detail =
        "{\"path\":\"" + util::jsonEscape(absolute) + "\",\"version\":" +
        std::to_string(version) + ",\"object\":\"" + object + "\",\"sha256\":\"" +
        util::hexEncode(result.sha256) + "\",\"size\":" +
        std::to_string(result.plaintextSize) + ",\"mtime\":" + std::to_string(mtime) +
        ",\"ts\":\"" + util::nowIso() + "\"}";
    audit::AuditLog log(m_directory / "index", m_password + "|index", m_logIterations);
    log.append("backup", "backup", detail);

    BackupEntry entry;
    entry.path = absolute;
    entry.version = version;
    entry.object = object;
    entry.sha256 = util::hexEncode(result.sha256);
    entry.size = result.plaintextSize;
    entry.mtime = mtime;
    entry.ts = util::nowIso();
    return entry;
}

BackupStore::RestoreResult BackupStore::restore(const std::string &path, std::uint32_t version,
                                                std::filesystem::path dest, bool force,
                                                std::filesystem::path destRoot,
                                                std::filesystem::path stripPrefix)
{
    const std::vector<BackupEntry> all = versions(path);
    if (all.empty())
        throw SecurityError(ErrorCode::InvalidArgument, "没有该文件的备份：" + path);

    const BackupEntry *entry = nullptr;
    if (version == 0) {
        entry = &all.back();
    } else {
        for (const auto &candidate : all) {
            if (candidate.version == version) {
                entry = &candidate;
                break;
            }
        }
    }
    if (!entry)
        throw SecurityError(ErrorCode::InvalidArgument, "该版本不存在");

    std::filesystem::path target;
    if (!dest.empty()) {
        target = dest;
    } else if (!destRoot.empty()) {
        const std::filesystem::path original = util::fromUtf8(entry->path);
        std::filesystem::path relative;
        if (!stripPrefix.empty()) {
            relative = original.lexically_relative(stripPrefix);
            const std::string relText = util::pathUtf8(relative);
            if (relative.empty() || relText.rfind("..", 0) == 0)
                relative = original.relative_path();
        } else {
            relative = original.relative_path();
        }
        target = destRoot / relative;
    } else {
        target = util::fromUtf8(entry->path);
    }
    const bool targetExists = std::filesystem::exists(target);

    if (targetExists && !force) {
        const std::string current = util::hexEncode(crypto::sha256File(target));
        bool tracked = false;
        for (const auto &candidate : all)
            tracked = tracked || candidate.sha256 == current;
        if (!tracked) {
            RestoreResult conflict;
            conflict.conflict = true;
            throw SecurityError(
                ErrorCode::InvalidArgument,
                "目标文件有未备份过的改动，拒绝覆盖（确认覆盖请加 --force，会先自动备份当前内容）");
        }
    }
    if (targetExists) {
        if (!force) {
            std::error_code ignored;
            std::filesystem::remove(target, ignored);
        } else {
            backupFile(target); // 覆盖前自动快照
            std::error_code ignored;
            std::filesystem::remove(target, ignored);
        }
    }

    std::filesystem::create_directories(target.parent_path());
    const crypto::Result result =
        crypto::decryptFile(objectPath(entry->object), target, objectPassword());
    if (!crypto::constantTimeEquals(result.sha256, util::hexDecode(entry->sha256))) {
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        throw SecurityError(ErrorCode::Integrity, "还原文件哈希与索引不一致");
    }

    RestoreResult out;
    out.path = entry->path;
    out.restoredTo = util::pathUtf8(target);
    out.sha256 = entry->sha256;
    out.version = entry->version;
    return out;
}

BackupStore::VerifyResult BackupStore::verify(bool full)
{
    VerifyResult result;
    try {
        audit::AuditLog log(m_directory / "index", m_password + "|index", m_logIterations);
        const auto verify = log.verify();
        if (!verify.ok) {
            result.problems = verify.problems;
            result.firstProblem = verify.firstProblem;
            return result;
        }
        if (!full) {
            result.ok = true;
            return result;
        }
        std::set<std::string> seen;
        for (const auto &entry : readIndex()) {
            if (!seen.insert(entry.object).second)
                continue;
            ++result.objects;
            try {
                const crypto::Result check =
                    crypto::verifyFile(objectPath(entry.object), objectPassword());
                if (!crypto::constantTimeEquals(check.sha256, util::hexDecode(entry.sha256))) {
                    ++result.problems;
                    if (result.firstProblem.empty())
                        result.firstProblem = entry.object + "：内容与索引不一致";
                }
            } catch (const SecurityError &error) {
                ++result.problems;
                if (result.firstProblem.empty())
                    result.firstProblem = entry.object + "：" + error.what();
            }
        }
    } catch (const SecurityError &error) {
        ++result.problems;
        result.firstProblem = error.what();
    }
    result.ok = result.problems == 0;
    return result;
}


namespace {

bool looksBinary(const crypto::Bytes &data)
{
    const std::size_t limit = std::min<std::size_t>(data.size(), 8192);
    std::size_t control = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (data[i] == 0)
            return true;
        if (data[i] < 0x09)
            ++control;
    }
    return control > 8;
}

std::vector<std::string> splitKeep(const std::string &text)
{
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (ch != '\r') {
            current.push_back(ch);
        }
    }
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

std::string unifiedDiff(const std::vector<std::string> &left, const std::vector<std::string> &right,
                        std::uint32_t fromVersion, std::uint32_t toVersion,
                        std::size_t maxLines, bool *truncated)
{
    const std::size_t n = left.size();
    const std::size_t m = right.size();
    if (n > 20000 || m > 20000 || n * m > 8000000) {
        *truncated = true;
        return "(文件过大，无法逐行对比)";
    }
    std::vector<int> dp((n + 1) * (m + 1), 0);
    const auto at = [&dp, m](std::size_t i, std::size_t j) -> int & {
        return dp[i * (m + 1) + j];
    };
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;)
            at(i, j) = (left[i] == right[j]) ? at(i + 1, j + 1) + 1
                                             : std::max(at(i + 1, j), at(i, j + 1));
    }

    struct Op {
        char kind;
        std::size_t index;
    };
    std::vector<Op> ops;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n && j < m) {
        if (left[i] == right[j]) {
            ops.push_back({' ', i});
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ops.push_back({'-', i});
            ++i;
        } else {
            ops.push_back({'+', j});
            ++j;
        }
    }
    while (i < n) {
        ops.push_back({'-', i});
        ++i;
    }
    while (j < m) {
        ops.push_back({'+', j});
        ++j;
    }

    std::vector<std::string> out;
    out.push_back("--- v" + std::to_string(fromVersion));
    out.push_back("+++ v" + std::to_string(toVersion));

    std::size_t pos = 0;
    while (pos < ops.size()) {
        while (pos < ops.size() && ops[pos].kind == ' ')
            ++pos;
        if (pos >= ops.size())
            break;
        const std::size_t changeStart = pos;
        while (pos < ops.size() && ops[pos].kind != ' ')
            ++pos;
        const std::size_t changeEnd = pos;

        const std::size_t hunkStart = changeStart > 2 ? changeStart - 2 : 0;
        const std::size_t hunkEnd = std::min(ops.size(), changeEnd + 2);

        std::size_t leftStart = 0;
        std::size_t rightStart = 0;
        for (std::size_t k = 0; k < hunkStart; ++k) {
            if (ops[k].kind != '+')
                ++leftStart;
            if (ops[k].kind != '-')
                ++rightStart;
        }
        std::size_t leftCount = 0;
        std::size_t rightCount = 0;
        for (std::size_t k = hunkStart; k < hunkEnd; ++k) {
            if (ops[k].kind != '+')
                ++leftCount;
            if (ops[k].kind != '-')
                ++rightCount;
        }
        out.push_back("@@" + std::to_string(fromVersion) + ":" + std::to_string(leftStart + 1) +
                      "," + std::to_string(leftCount) + " -> " + std::to_string(rightStart + 1) +
                      "," + std::to_string(rightCount) + "@@");
        for (std::size_t k = hunkStart; k < hunkEnd; ++k) {
            const Op &op = ops[k];
            if (op.kind == ' ')
                out.push_back(" " + left[op.index]);
            else if (op.kind == '-')
                out.push_back("-" + left[op.index]);
            else
                out.push_back("+" + right[op.index]);
        }
        if (out.size() > maxLines) {
            *truncated = true;
            out.resize(maxLines);
            break;
        }
    }
    std::string text;
    for (const std::string &line : out)
        text += line + "\n";
    return text;
}

} // namespace

BackupStore::DiffResult BackupStore::diff(const std::string &path, std::uint32_t fromVersion,
                                          std::uint32_t toVersion, std::size_t maxBytes,
                                          std::size_t maxLines)
{
    const std::vector<BackupEntry> all = versions(path);
    if (all.empty())
        throw SecurityError(ErrorCode::InvalidArgument, "没有该文件的备份：" + path);
    const BackupEntry *to = nullptr;
    if (toVersion == 0) {
        to = &all.back();
    } else {
        for (const auto &candidate : all)
            if (candidate.version == toVersion) {
                to = &candidate;
                break;
            }
    }
    if (!to)
        throw SecurityError(ErrorCode::InvalidArgument, "目标版本不存在");

    const BackupEntry *from = nullptr;
    if (fromVersion == 0) {
        for (const auto &candidate : all)
            if (candidate.version < to->version)
                from = &candidate;
        if (!from)
            throw SecurityError(ErrorCode::InvalidArgument, "没有更早的版本可对比（至少需要两个版本）");
    } else {
        for (const auto &candidate : all)
            if (candidate.version == fromVersion) {
                from = &candidate;
                break;
            }
        if (!from)
            throw SecurityError(ErrorCode::InvalidArgument, "起始版本不存在");
    }

    DiffResult result;
    result.path = from->path;
    result.fromVersion = from->version;
    result.toVersion = to->version;
    result.fromSha = from->sha256;
    result.toSha = to->sha256;
    result.fromSize = from->size;
    result.toSize = to->size;
    result.changed = from->sha256 != to->sha256;
    if (!result.changed) {
        result.kind = "none";
        return result;
    }

    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() /
        ("fg-diff-" + std::to_string(::GetCurrentProcessId()) + "-" + util::randomHex(4));
    std::error_code ignored;
    std::filesystem::create_directories(tempDir, ignored);
    const std::filesystem::path left = tempDir / "left.bin";
    const std::filesystem::path right = tempDir / "right.bin";
    try {
        crypto::decryptFile(objectPath(from->object), left, objectPassword());
        crypto::decryptFile(objectPath(to->object), right, objectPassword());
        const crypto::Bytes leftBytes = util::readAll(left);
        const crypto::Bytes rightBytes = util::readAll(right);
        if (leftBytes.size() > maxBytes || rightBytes.size() > maxBytes ||
            looksBinary(leftBytes) || looksBinary(rightBytes)) {
            result.kind = "binary";
            result.truncated = leftBytes.size() > maxBytes || rightBytes.size() > maxBytes;
        } else {
            result.kind = "text";
            result.text = unifiedDiff(splitKeep(std::string(leftBytes.begin(), leftBytes.end())),
                                      splitKeep(std::string(rightBytes.begin(), rightBytes.end())),
                                      from->version, to->version, maxLines, &result.truncated);
        }
    } catch (...) {
        std::filesystem::remove_all(tempDir, ignored);
        throw;
    }
    std::filesystem::remove_all(tempDir, ignored);
    return result;
}

std::size_t BackupStore::purge(std::uint32_t keep)
{
    const std::vector<BackupEntry> all = readIndex();
    if (all.empty())
        return 0;

    std::map<std::string, std::vector<BackupEntry>> grouped;
    for (const auto &entry : all)
        grouped[entry.path].push_back(entry);

    std::vector<BackupEntry> kept;
    std::size_t pruned = 0;
    for (auto &item : grouped) {
        auto &list = item.second;
        std::sort(list.begin(), list.end(),
                  [](const BackupEntry &a, const BackupEntry &b) { return a.version < b.version; });
        const std::size_t dropCount = list.size() > keep ? list.size() - keep : 0;
        pruned += dropCount;
        for (std::size_t i = dropCount; i < list.size(); ++i)
            kept.push_back(list[i]);
    }
    if (pruned == 0)
        return 0;

    // 索引压缩：写新目录（同一加密索引格式），成功后原子换目录，旧目录删除。
    const std::string suffix = "-" + util::randomHex(6);
    const std::filesystem::path fresh = m_directory / ("index.new" + suffix);
    const std::filesystem::path old = m_directory / ("index.old" + suffix);
    std::error_code ignored;
    std::filesystem::create_directories(fresh, ignored);
    {
        audit::AuditLog log(fresh, m_password + "|index", m_logIterations);
        for (const auto &entry : kept) {
            const std::string detail =
                "{\"path\":\"" + util::jsonEscape(entry.path) + "\",\"version\":" +
                std::to_string(entry.version) + ",\"object\":\"" + entry.object +
                "\",\"sha256\":\"" + entry.sha256 + "\",\"size\":" +
                std::to_string(entry.size) + ",\"mtime\":" + std::to_string(entry.mtime) +
                ",\"ts\":\"" + entry.ts + "\"}";
            log.append("backup", "backup", detail);
        }
    }
    const std::filesystem::path indexDir = m_directory / "index";
    std::filesystem::rename(indexDir, old, ignored);
    std::filesystem::rename(fresh, indexDir, ignored);
    std::filesystem::remove_all(old, ignored);
    return pruned;
}

std::size_t BackupStore::gc()
{
    std::set<std::string> referenced;
    for (const auto &entry : readIndex())
        referenced.insert(entry.object);

    std::size_t removed = 0;
    const std::filesystem::path objects = m_directory / kObjectsDir;
    std::error_code error;
    if (!std::filesystem::exists(objects, error))
        return 0;

    for (const auto &dirEntry : std::filesystem::recursive_directory_iterator(objects, error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!dirEntry.is_regular_file())
            continue;
        const std::string name = util::pathUtf8(dirEntry.path().stem());
        if (name.size() != 64)
            continue;
        if (referenced.count(util::pathUtf8(dirEntry.path().stem())) == 0) {
            std::error_code ignored;
            std::filesystem::remove(dirEntry.path(), ignored);
            ++removed;
        }
    }
    return removed;
}

std::size_t BackupStore::objectCount() const
{
    std::size_t count = 0;
    std::error_code error;
    const std::filesystem::path objects = m_directory / kObjectsDir;
    if (!std::filesystem::exists(objects, error))
        return 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(objects, error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (entry.is_regular_file())
            ++count;
    }
    return count;
}

} // namespace file_guard
