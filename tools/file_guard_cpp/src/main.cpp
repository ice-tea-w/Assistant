#include "file_guard/audit.h"
#include "file_guard/backup.h"
#include "file_guard/config.h"
#include "file_guard/crypto.h"
#include "file_guard/integrity.h"
#include "file_guard/scan.h"
#include "file_guard/service.h"
#include "file_guard/util.h"
#include "file_guard/vault.h"
#include "file_guard/watch.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace file_guard;

struct Args {
    std::vector<std::string> positional;
    std::string home;
    std::string password;
    std::string keyfile;
    bool json = false;
    bool quiet = false;
    bool confirm = false;
    bool allowRemote = false;
    bool once = false;
    bool force = false;
    bool removeSource = false;
    std::uint32_t iterations = 0;
    std::uint32_t version = 0;
    int port = 0;
    int autoEncryptSeconds = 0;
    std::string dst;
    std::string dest;
    std::string event;
    std::string detail;
    std::string note;
    std::string minRisk;
    std::string recoveryCode;
    std::string destRoot;
    std::string stripPrefix;
    std::uint32_t keep = 0;
    std::uint32_t fromVersion = 0;
    bool noDpapi = false;
    bool autoBackup = false;
};

std::string stripDashes(const std::string &text)
{
    if (text.rfind("--", 0) == 0)
        return text.substr(2);
    return {};
}

Args parseArgs(const std::vector<std::string> &arguments)
{
    Args args;
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        const std::string arg = arguments[i];
        const std::string key = stripDashes(arg);
        if (key.empty()) {
            args.positional.push_back(arg);
            continue;
        }
        auto next = [&](std::string *out) {
            if (i + 1 < arguments.size())
                *out = arguments[++i];
        };
        std::string value;
        if (key == "home") { next(&args.home); }
        else if (key == "password") { next(&args.password); }
        else if (key == "keyfile") { next(&args.keyfile); }
        else if (key == "json") { args.json = true; }
        else if (key == "quiet") { args.quiet = true; }
        else if (key == "confirm") { args.confirm = true; }
        else if (key == "allow-remote") { args.allowRemote = true; }
        else if (key == "once") { args.once = true; }
        else if (key == "force") { args.force = true; }
        else if (key == "remove-source") { args.removeSource = true; }
        else if (key == "iterations") { next(&value); args.iterations = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)); }
        else if (key == "version") { next(&value); args.version = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)); }
        else if (key == "port") { next(&value); args.port = std::atoi(value.c_str()); }
        else if (key == "auto-encrypt") { next(&value); args.autoEncryptSeconds = std::atoi(value.c_str()); }
        else if (key == "dst") { next(&args.dst); }
        else if (key == "dest") { next(&args.dest); }
        else if (key == "event") { next(&args.event); }
        else if (key == "detail") { next(&args.detail); }
        else if (key == "note") { next(&args.note); }
        else if (key == "min-risk") { next(&args.minRisk); }
        else if (key == "recovery-code") { next(&args.recoveryCode); }
        else if (key == "dest-root") { next(&args.destRoot); }
        else if (key == "strip-prefix") { next(&args.stripPrefix); }
        else if (key == "keep") { next(&value); args.keep = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)); }
        else if (key == "from") { next(&value); args.fromVersion = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)); }
        else if (key == "no-dpapi") { args.noDpapi = true; }
        else if (key == "auto-backup") { args.autoBackup = true; }
        else {
            std::cerr << "未知参数：" << arg << "\n";
            std::exit(2);
        }
    }
    return args;
}

std::string resolvePassword(const Args &args, const Config &config, bool confirm = false)
{
    if (!args.keyfile.empty()) {
        const crypto::Bytes material = crypto::sha256(util::readAll(util::fromUtf8(args.keyfile)));
        return std::string(material.begin(), material.end());
    }
    if (!args.password.empty())
        return args.password;
    const std::string env = util::envOr("FG_PASSWORD", "");
    if (!env.empty())
        return env;
    const std::filesystem::path cache = config.home / "dpapi.bin";
    if (std::filesystem::is_regular_file(cache)) {
        try {
            const crypto::Bytes cached = util::dpapiUnprotect(util::readAll(cache));
            return std::string(cached.begin(), cached.end());
        } catch (...) {
            // 忽略，转交互
        }
    }
    if (!args.json) {
        std::cout << "请输入密码：";
        std::string password;
        std::getline(std::cin, password);
        if (confirm) {
            std::cout << "请再次输入：";
            std::string again;
            std::getline(std::cin, again);
            if (again != password)
                throw SecurityError(ErrorCode::Password, "两次输入的密码不一致");
        }
        if (!password.empty())
            return password;
    }
    throw SecurityError(ErrorCode::Password, "需要密码（--password / FG_PASSWORD / 交互输入）");
}

Config loadConfig(const Args &args)
{
    Config config = Config::load(args.home.empty() ? std::filesystem::path() : util::fromUtf8(args.home));
    if (args.iterations)
        config.iterations = args.iterations;
    if (args.port > 0)
        config.port = args.port;
    return config;
}

std::string humanSize(std::uint64_t bytes)
{
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes / (1024 * 1024)) + " MB";
}

// ---- JSON 输出辅助（供主程序 SecurityBridge 消费）----

int severityRank(scan::Severity severity)
{
    switch (severity) {
    case scan::Severity::High:
        return 3;
    case scan::Severity::Medium:
        return 2;
    case scan::Severity::Low:
        return 1;
    default:
        return 0;
    }
}

const char *severityKey(scan::Severity severity)
{
    switch (severity) {
    case scan::Severity::High:
        return "high";
    case scan::Severity::Medium:
        return "medium";
    case scan::Severity::Low:
        return "low";
    default:
        return "info";
    }
}

const char *errorTypeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::InvalidArgument:
        return "ArgumentError";
    case ErrorCode::Password:
        return "PasswordError";
    case ErrorCode::Corrupt:
        return "IntegrityError";
    case ErrorCode::Integrity:
        return "IntegrityError";
    case ErrorCode::Io:
        return "IoError";
    case ErrorCode::Platform:
        return "PlatformError";
    }
    return "Error";
}

int cmdInit(const Args &args)
{
    Config config = loadConfig(args);
    config.ensureDirs();
    if (args.iterations)
        config.iterations = args.iterations;
    config.save();

    if (!args.noDpapi) {
        std::string password = args.password;
        if (password.empty())
            password = util::envOr("FG_PASSWORD", "");
        if (!password.empty()) {
            try {
                util::writeAll(config.home / "dpapi.bin",
                               util::dpapiProtect(crypto::Bytes(password.begin(), password.end())));
                util::restrictAcl(config.home / "dpapi.bin");
                std::cout << "已启用 DPAPI 免密（当前 Windows 用户）\n";
            } catch (const std::exception &ex) {
                std::cout << "DPAPI 未启用：" << ex.what() << "\n";
            }
        }
    }
    std::cout << "初始化完成：" << util::pathUtf8(config.home) << "\n";
    return 0;
}

int cmdEncrypt(const Args &args)
{
    Config config = loadConfig(args);
    const std::string password = resolvePassword(args, config);
    if (args.positional.empty())
        throw SecurityError(ErrorCode::InvalidArgument, "加密需要源文件");
    const std::filesystem::path src = util::fromUtf8(args.positional[0]);

    if (std::filesystem::is_directory(src)) {
        const std::filesystem::path dstRoot = args.dst.empty()
                                                  ? std::filesystem::path(src.wstring() + L".enc")
                                                  : util::fromUtf8(args.dst);
        std::size_t done = 0;
        for (const auto &file : collectFiles(src, true)) {
            const std::filesystem::path relative = std::filesystem::relative(file, src);
            const std::filesystem::path dst =
                std::filesystem::path((dstRoot / relative).wstring() + L".fgc");
            std::error_code ignored;
            if (std::filesystem::exists(dst)) {
                if (!args.force) {
                    std::cout << "[跳过] 已存在：" << util::pathUtf8(dst) << "\n";
                    continue;
                }
                std::filesystem::remove(dst, ignored);
            }
            std::filesystem::create_directories(dst.parent_path(), ignored);
            crypto::encryptFile(file, dst, password, config.iterations, config.chunkSize);
            ++done;
        }
        std::cout << "[OK] 目录加密完成：" << done << " 个文件 -> " << util::pathUtf8(dstRoot) << "\n";
        return 0;
    }

    const std::filesystem::path dst = args.dst.empty()
                                          ? std::filesystem::path(src.wstring() + L".fgc")
                                          : util::fromUtf8(args.dst);
    const crypto::Result result =
        crypto::encryptFile(src, dst, password, config.iterations, config.chunkSize);
    std::cout << "[OK] " << util::pathUtf8(src) << " -> " << util::pathUtf8(dst) << "（"
              << humanSize(result.plaintextSize) << "，sha256="
              << util::hexEncode(result.sha256).substr(0, 16) << "…）\n";
    return 0;
}

int cmdDecrypt(const Args &args)
{
    Config config = loadConfig(args);
    const std::string password = resolvePassword(args, config);
    if (args.positional.empty())
        throw SecurityError(ErrorCode::InvalidArgument, "解密需要容器路径");
    if (std::filesystem::is_directory(util::fromUtf8(args.positional[0]))) {
        const std::filesystem::path srcRoot = util::fromUtf8(args.positional[0]);
        const std::filesystem::path dstRoot = args.dst.empty()
                                                  ? std::filesystem::path(srcRoot.wstring() + L".dec")
                                                  : util::fromUtf8(args.dst);
        std::size_t done = 0;
        std::error_code ignored;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(
                 srcRoot, std::filesystem::directory_options::skip_permission_denied, ignored)) {
            if (ignored) {
                ignored.clear();
                continue;
            }
            if (!entry.is_regular_file())
                continue;
            const std::string name = util::pathUtf8(entry.path());
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".fgc") != 0)
                continue;
            const std::filesystem::path relative = std::filesystem::relative(entry.path(), srcRoot);
            std::wstring outName = (dstRoot / relative).wstring();
            outName.resize(outName.size() - 4);
            const std::filesystem::path out(outName);
            if (std::filesystem::exists(out)) {
                if (!args.force)
                    continue;
                std::filesystem::remove(out, ignored);
            }
            std::filesystem::create_directories(out.parent_path(), ignored);
            crypto::decryptFile(entry.path(), out, password);
            ++done;
        }
        std::cout << "[OK] 目录解密完成：" << done << " 个文件 -> " << util::pathUtf8(dstRoot) << "\n";
        return 0;
    }

    std::filesystem::path dst = args.dst.empty() ? std::filesystem::path() : util::fromUtf8(args.dst);
    if (dst.empty()) {
        std::string name = util::pathUtf8(util::fromUtf8(args.positional[0]));
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".fgc") == 0)
            name.resize(name.size() - 4);
        dst = util::fromUtf8(name + ".out");
    }
    std::error_code ignored;
    if (std::filesystem::exists(dst)) {
        if (!args.force)
            throw SecurityError(ErrorCode::InvalidArgument, "目标已存在（加 --force 覆盖）");
        std::filesystem::remove(dst, ignored);
    }
    const crypto::Result result =
        crypto::decryptFile(util::fromUtf8(args.positional[0]), dst, password);
    std::cout << "[OK] 解密完成 -> " << util::pathUtf8(dst) << "（sha256="
              << util::hexEncode(result.sha256).substr(0, 16) << "…）\n";
    return 0;
}

int cmdVerify(const Args &args)
{
    Config config = loadConfig(args);
    const std::string password = resolvePassword(args, config);
    const crypto::Result result =
        crypto::verifyFile(util::fromUtf8(args.positional.at(0)), password);
    std::cout << "校验通过：" << util::pathUtf8(args.positional[0]) << "（sha256="
              << util::hexEncode(result.sha256) << "）\n";
    return 0;
}

int cmdInfo(const Args &args)
{
    const crypto::ContainerInfo info = crypto::inspect(util::fromUtf8(args.positional.at(0)));
    std::cout << "容器信息：" << util::pathUtf8(args.positional[0]) << "\n"
              << "  迭代数：" << info.iterations << "\n  分块：" << info.chunkSize
              << "\n  明文长度：" << info.plaintextSize << "\n";
    return 0;
}

int cmdScan(const Args &args)
{
    if (args.positional.empty())
        throw SecurityError(ErrorCode::InvalidArgument, "用法：scan <路径> [--min-risk high|medium|low]");
    int minRank = 0;
    if (args.minRisk == "high")
        minRank = 3;
    else if (args.minRisk == "medium")
        minRank = 2;
    else if (args.minRisk == "low")
        minRank = 1;

    const scan::Report report = scan::scanPath(util::fromUtf8(args.positional.at(0)), true);
    std::size_t high = 0;
    std::size_t medium = 0;
    std::size_t low = 0;
    std::string items;
    for (const auto &file : report.results) {
        std::string findings;
        for (const auto &finding : file.findings) {
            if (severityRank(finding.severity) < minRank)
                continue;
            if (finding.severity == scan::Severity::High)
                ++high;
            else if (finding.severity == scan::Severity::Medium)
                ++medium;
            else if (finding.severity == scan::Severity::Low)
                ++low;
            if (!findings.empty())
                findings += ",";
            findings += "{\"rule\":\"" + util::jsonEscape(finding.rule) +
                        "\",\"severity\":\"" + severityKey(finding.severity) +
                        "\",\"sample\":\"" + util::jsonEscape(finding.sample) +
                        "\",\"line\":" + std::to_string(finding.line) + "}";
        }
        if (!findings.empty()) {
            if (!items.empty())
                items += ",";
            items += "{\"path\":\"" + util::jsonEscape(util::pathUtf8(file.path)) +
                     "\",\"findings\":[" + findings + "]}";
        }
        if (!args.json && file.skipped && !args.quiet)
            std::cout << "[跳过] " << util::pathUtf8(file.path) << "  " << file.reason << "\n";
    }

    if (args.json) {
        std::cout << "{\"summary\":{\"files\":" << report.filesScanned
                  << ",\"skipped\":" << report.filesSkipped
                  << ",\"high\":" << high
                  << ",\"medium\":" << medium
                  << ",\"low\":" << low
                  << "},\"files\":[" << items << "]}\n";
    } else {
        for (const auto &file : report.results) {
            for (const auto &finding : file.findings) {
                if (severityRank(finding.severity) < minRank)
                    continue;
                std::cout << "[" << (finding.severity == scan::Severity::High ? "高" :
                                     finding.severity == scan::Severity::Medium ? "中" : "低")
                          << "] " << util::pathUtf8(file.path) << "  " << finding.rule
                          << "  " << finding.sample << "\n";
            }
        }
        std::cout << "扫描完成：文件 " << report.filesScanned << "，跳过 " << report.filesSkipped
                  << "，高 " << high << " / 中 " << medium << " / 低 " << low << "\n";
    }
    return (high + medium) > 0 ? 1 : 0;
}

int cmdLog(const Args &args)
{
    Config config = loadConfig(args);
    const std::string password = resolvePassword(args, config);
    const std::string action = args.positional.empty() ? "" : args.positional[0];
    audit::AuditLog log(config.auditDir(), password, config.logIterations);
    if (action == "record") {
        log.append(args.event.empty() ? "manual" : args.event, "cli", args.detail);
        std::cout << "[OK] 已记录审计事件\n";
        return 0;
    }
    if (action == "head") {
        std::cout << "链头：" << log.head() << "\n";
        return 0;
    }
    if (action == "export") {
        std::string out;
        for (const auto &record : log.tail(10'000'000))
            out += "{\"seq\":" + std::to_string(record.seq) + ",\"event\":\"" +
                   util::jsonEscape(record.event) + "\",\"detail\":\"" +
                   util::jsonEscape(record.detail) + "\",\"ts\":\"" + record.timestamp +
                   "\"}\n";
        if (args.dst.empty())
            std::cout << out;
        else
            util::writeText(util::fromUtf8(args.dst), out);
        return 0;
    }
    if (action == "clear") {
        const std::filesystem::path dir = config.auditDir();
        std::string stamp = util::nowIso();
        std::replace(stamp.begin(), stamp.end(), ':', '-');
        const std::filesystem::path archive = dir / "archive" / stamp;
        std::error_code ignored;
        std::filesystem::create_directories(archive, ignored);
        for (const char *name : {"audit.jsonl", "audit.meta.json"}) {
            const std::filesystem::path from = dir / name;
            if (std::filesystem::exists(from))
                std::filesystem::rename(from, archive / name, ignored);
        }
        std::cout << "[OK] 审计日志已归档到 " << util::pathUtf8(archive) << "（新日志从零开始）\n";
        return 0;
    }
    if (action == "verify") {
        const audit::VerifyResult result = log.verify();
        if (args.json) {
            std::cout << "{\"ok\":" << (result.ok ? "true" : "false")
                      << ",\"records\":" << result.records
                      << ",\"head\":\"" << util::jsonEscape(result.head) << "\""
                      << ",\"problems\":" << result.problems << "}\n";
        } else {
            std::cout << (result.ok ? "[OK] 审计链校验通过" : "[X] 审计链校验失败")
                      << "：记录 " << result.records << " 条";
            if (!result.ok)
                std::cout << "（" << result.firstProblem << "）";
            std::cout << "\n";
        }
        return result.ok ? 0 : 5;
    }
    for (const auto &record : log.tail(20))
        std::cout << record.seq << "  " << record.timestamp << "  " << record.event << "  "
                  << record.detail << "\n";
    return 0;
}

int cmdBackup(const Args &args)
{
    Config config = loadConfig(args);
    const std::string password = resolvePassword(args, config);
    BackupStore store(config.backupDir(), password, config.iterations, config.logIterations);
    const std::string action = args.positional.empty() ? "" : args.positional[0];
    if (action == "add") {
        const BackupEntry entry = store.backupFile(util::fromUtf8(args.positional.at(1)));
        std::cout << "[OK] 已备份 " << entry.path << "（版本 " << entry.version << "，"
                  << humanSize(entry.size) << "）\n";
        return 0;
    }
    if (action == "list") {
        const auto entries = store.list();
        if (args.json) {
            std::string items;
            for (const auto &entry : entries) {
                if (!items.empty())
                    items += ",";
                items += "{\"path\":\"" + util::jsonEscape(entry.path) +
                         "\",\"version\":" + std::to_string(entry.version) +
                         ",\"size\":" + std::to_string(entry.size) +
                         ",\"sha256\":\"" + util::jsonEscape(entry.sha256) +
                         "\",\"mtime\":" + std::to_string(entry.mtime) + "}";
            }
            std::cout << "{\"items\":[" << items << "]}\n";
            return 0;
        }
        for (const auto &entry : entries)
            std::cout << entry.path << "  v" << entry.version << "  " << humanSize(entry.size)
                      << "  " << entry.sha256.substr(0, 12) << "…\n";
        return 0;
    }
    if (action == "restore") {
        const std::string path = args.positional.at(1);
        const auto result = store.restore(
            path, args.version,
            args.dest.empty() ? std::filesystem::path() : util::fromUtf8(args.dest), args.force,
            args.destRoot.empty() ? std::filesystem::path() : util::fromUtf8(args.destRoot),
            args.stripPrefix.empty() ? std::filesystem::path()
                                     : util::fromUtf8(args.stripPrefix));
        std::cout << "[OK] 已还原 " << path << "（v" << result.version << "）-> "
                  << result.restoredTo << "\n";
        return 0;
    }
    if (action == "diff") {
        if (args.positional.size() < 2)
            throw SecurityError(ErrorCode::InvalidArgument,
                                "用法：backup diff <path> [--from N] [--version N]");
        const auto result = store.diff(
            util::pathUtf8(std::filesystem::absolute(util::fromUtf8(args.positional[1]))),
            args.fromVersion, args.version);
        std::cout << "差异：" << result.path << "  v" << result.fromVersion << " -> v"
                  << result.toVersion << "（" << result.kind << "）";
        if (!result.changed) {
            std::cout << " 未变化\n";
            return 0;
        }
        std::cout << "\n" << result.text;
        if (result.truncated)
            std::cout << "...（已截断）\n";
        return 0;
    }
    if (action == "purge") {
        if (args.keep == 0)
            throw SecurityError(ErrorCode::InvalidArgument, "用法：backup purge --keep N");
        const std::size_t pruned = store.purge(args.keep);
        const std::size_t removed = store.gc();
        std::cout << "[OK] 已清理旧版本 " << pruned << " 个，删除未引用对象 " << removed << " 个\n";
        return 0;
    }
    if (action == "verify") {
        const auto result = store.verify(true);
        std::cout << (result.ok ? "[OK] 备份校验通过" : "[X] 备份校验失败") << "：对象 "
                  << result.objects << " 个";
        if (!result.ok)
            std::cout << "（" << result.firstProblem << "）";
        std::cout << "\n";
        return result.ok ? 0 : 5;
    }
    if (action == "gc") {
        const std::size_t removed = store.gc();
        std::cout << "[OK] 清理未引用对象 " << removed << " 个\n";
        return 0;
    }
    throw SecurityError(ErrorCode::InvalidArgument, "用法：backup add|list|restore|verify|gc");
}

int cmdPasswd(const Args &args)
{
    Config config = loadConfig(args);
    if (args.positional.size() < 2)
        throw SecurityError(ErrorCode::InvalidArgument,
                            "用法：passwd <容器> <新密码>（容器口令重新加密）");
    const std::filesystem::path container = util::fromUtf8(args.positional[0]);
    const std::string oldPassword = resolvePassword(args, config);
    const std::string newPassword = args.positional[1];
    const std::filesystem::path tmp = std::filesystem::path(container.wstring() + L".plain.tmp");
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    crypto::decryptFile(container, tmp, oldPassword);
    const std::filesystem::path newContainer = std::filesystem::path(container.wstring() + L".new.tmp");
    std::filesystem::remove(newContainer, ignored);
    crypto::encryptFile(tmp, newContainer, newPassword, config.iterations, config.chunkSize);
    std::filesystem::remove(tmp, ignored);
    std::filesystem::remove(container, ignored);
    std::filesystem::rename(newContainer, container);
    std::cout << "[OK] 已用新口令重新加密：" << util::pathUtf8(container) << "\n";
    return 0;
}

int cmdKeygen(const Args &args)
{
    Config config = loadConfig(args);
    const std::filesystem::path out =
        args.dst.empty() ? config.home / "keyfile.bin" : util::fromUtf8(args.dst);
    std::filesystem::create_directories(out.parent_path());
    const crypto::Bytes material = crypto::randomBytes(32);
    util::writeAll(out, material);
    util::restrictAcl(out);
    std::cout << "[OK] 已生成 32 字节密钥文件：" << util::pathUtf8(out) << "\n"
              << "用法：--keyfile \"" << util::pathUtf8(out) << "\"（与 --password 二选一）\n";
    return 0;
}

int cmdVault(const Args &args)
{
    Config config = loadConfig(args);
    Vault vault(config.vaultDir(), config.iterations, config.logIterations);
    const std::string action = args.positional.empty() ? "" : args.positional[0];

    // info 不强制口令：没口令也能报告容器数/DPAPI，文件数才需要解锁。
    if (action == "info") {
        const bool exists = vault.exists();
        std::size_t containers = 0;
        std::error_code ec;
        const std::filesystem::path containersDir = config.vaultDir() / "containers";
        if (std::filesystem::is_directory(containersDir, ec)) {
            for (const auto &entry : std::filesystem::directory_iterator(containersDir, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".fgc")
                    ++containers;
            }
        }
        bool dpapi = false;
        if (exists) {
            const std::string keys = util::readText(config.vaultDir() / "vault.keys");
            const std::string marker = "dpapi_dek=";
            const std::size_t pos = keys.find(marker);
            if (pos != std::string::npos) {
                const std::size_t valueStart = pos + marker.size();
                dpapi = valueStart < keys.size() && keys[valueStart] != '\r' &&
                        keys[valueStart] != '\n';
            }
        }
        std::size_t files = 0;
        if (exists) {
            try {
                if (!args.recoveryCode.empty())
                    vault.unlockWithRecovery(args.recoveryCode);
                else
                    vault.unlock(resolvePassword(args, config));
                files = vault.list().size();
            } catch (const std::exception &) {
                files = 0;   // 没有口令就只报容器数
            }
        }
        if (args.json) {
            std::cout << "{\"directory\":\"" << util::jsonEscape(util::pathUtf8(config.vaultDir()))
                      << "\",\"exists\":" << (exists ? "true" : "false")
                      << ",\"files\":" << files
                      << ",\"containers\":" << containers
                      << ",\"dpapi\":" << (dpapi ? "true" : "false") << "}\n";
        } else {
            std::cout << "保险箱：" << util::pathUtf8(config.vaultDir()) << "\n"
                      << "  文件：" << files << "，容器：" << containers
                      << "，DPAPI：" << (dpapi ? "已启用" : "未启用") << "\n";
        }
        return 0;
    }

    const std::string password =
        action == "create" ? resolvePassword(args, config, true)
                           : (args.recoveryCode.empty() ? resolvePassword(args, config) : std::string());

    if (action == "create") {
        const auto result = vault.create(password, true);
        std::cout << "保险箱已创建：" << util::pathUtf8(config.vaultDir()) << "\n"
                  << "恢复码（只显示这一次，请抄到离线位置）：\n  " << result.recoveryCode
                  << "\n注意：恢复码 = 第二把钥匙；丢失且忘记口令 = 数据不可恢复。\n";
        return 0;
    }
    if (!args.recoveryCode.empty())
        vault.unlockWithRecovery(args.recoveryCode);
    else
        vault.unlock(password);

    if (action == "add") {
        const VaultFileInfo info =
            vault.addFile(util::fromUtf8(args.positional.at(1)), args.note, args.removeSource);
        std::cout << "[OK] 已放入保险箱：" << info.name << " -> " << info.container << "\n";
        return 0;
    }
    if (action == "list") {
        const auto files = vault.list();
        if (files.empty())
            std::cout << "保险箱为空\n";
        for (std::size_t i = 0; i < files.size(); ++i)
            std::cout << (i + 1) << ". " << files[i].name << "  " << humanSize(files[i].size)
                      << "  " << files[i].container << "\n";
        return 0;
    }
    if (action == "extract") {
        const auto result = vault.extract(
            args.positional.at(1),
            args.dest.empty() ? std::filesystem::path() : util::fromUtf8(args.dest), args.force);
        std::cout << "[OK] 已取出 " << result.name << " -> " << result.restoredTo << "\n";
        return 0;
    }
    if (action == "verify") {
        const auto result = vault.verify();
        std::cout << (result.ok ? "[OK] 保险箱校验通过" : "[X] 保险箱校验失败") << "：文件 "
                  << result.files << " 个";
        if (!result.ok)
            std::cout << "（" << result.firstProblem << "）";
        std::cout << "\n";
        return result.ok ? 0 : 5;
    }
    if (action == "recovery") {
        std::cout << "恢复码：\n  " << vault.recoveryCode(password) << "\n";
        return 0;
    }
    if (action == "passwd") {
        if (args.positional.size() < 2)
            throw SecurityError(ErrorCode::InvalidArgument, "用法：vault passwd <新口令>");
        vault.changePassword(password, args.positional[1]);
        std::cout << "[OK] 口令已轮换（容器与清单未重加密）\n";
        return 0;
    }
    return 0;
}

int cmdWatch(const Args &args)
{
    Config config = loadConfig(args);
    if (args.positional.empty())
        throw SecurityError(ErrorCode::InvalidArgument, "watch 需要文件或目录");
    if (!args.autoBackup && args.autoEncryptSeconds <= 0)
        throw SecurityError(ErrorCode::InvalidArgument,
                            "watch 至少需要 --auto-backup 或 --auto-encrypt 秒数 之一");

    const std::string password = resolvePassword(args, config);
    std::vector<std::filesystem::path> roots;
    std::map<std::filesystem::path, bool> watchDirs;   // dir -> 是否整目录监听
    std::set<std::filesystem::path> watchFiles;        // 文件参数（精确匹配）

    for (const auto &item : args.positional) {
        const std::filesystem::path path = std::filesystem::absolute(util::fromUtf8(item));
        roots.push_back(path);
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            watchDirs[path] = true;
        } else {
            watchFiles.insert(path);
            watchDirs[path.parent_path()] = watchDirs.count(path.parent_path()) ? watchDirs[path.parent_path()] : false;
        }
    }

    BackupStore store(config.backupDir(), password, config.iterations, config.logIterations);
    std::mutex printMutex;
    std::mutex storeMutex;
    std::vector<std::unique_ptr<DirectoryWatcher>> watchers;

    if (args.autoBackup) {
        for (const auto &item : watchDirs) {
            const std::filesystem::path dir = item.first;
            const bool wholeDir = item.second;
            auto watcher = std::make_unique<DirectoryWatcher>(dir, true);
            std::string error;
            if (!watcher->start([&, wholeDir](const WatchEvent &event) {
                    if (!wholeDir && watchFiles.count(event.path) == 0)
                        return;
                    std::error_code ignored;
                    if (!std::filesystem::is_regular_file(event.path, ignored))
                        return;
                    try {
                        std::lock_guard<std::mutex> lock(storeMutex);
                        const BackupEntry entry = store.backupFile(event.path);
                        std::lock_guard<std::mutex> printLock(printMutex);
                        std::cout << "[备份] " << util::pathUtf8(event.path) << " v"
                                  << entry.version << "\n";
                    } catch (const std::exception &ex) {
                        std::lock_guard<std::mutex> printLock(printMutex);
                        std::cerr << "[跳过] " << ex.what() << "\n";
                    }
                }, &error)) {
                std::lock_guard<std::mutex> printLock(printMutex);
                std::cerr << error << "\n";
            } else {
                watchers.push_back(std::move(watcher));
            }
        }
    }

    AutoEncryptOptions autoOptions;
    autoOptions.password = password;
    autoOptions.idleSeconds = args.autoEncryptSeconds > 0 ? args.autoEncryptSeconds : 60.0;
    autoOptions.iterations = config.iterations;
    autoOptions.chunkSize = config.chunkSize;

    std::cout << "监听中：" << roots.size() << " 个路径（Ctrl+C 退出）";
    if (args.autoBackup)
        std::cout << "，变更自动备份";
    if (args.autoEncryptSeconds > 0)
        std::cout << "，空闲 " << args.autoEncryptSeconds << " 秒自动加密";
    std::cout << "\n";

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (args.autoEncryptSeconds <= 0)
            continue;
        const auto results = autoEncryptIdle(roots, autoOptions);
        for (const auto &result : results) {
            std::lock_guard<std::mutex> printLock(printMutex);
            if (result.action == "encrypted")
                std::cout << "[自动加密] " << util::pathUtf8(result.path) << " -> "
                          << result.dst << "\n";
            else if (result.action == "error")
                std::cerr << "[自动加密失败] " << util::pathUtf8(result.path) << " "
                          << result.error << "\n";
        }
    }
    return 0;
}

int cmdServe(const Args &args)
{
    Config config = loadConfig(args);
    ServiceOptions options;
    options.port = args.port > 0 ? args.port : config.port;
    options.allowRemote = args.allowRemote;
    options.once = args.once;
    LocalService service(config, options);
    return service.run();
}

int cmdIntegrity(const Args &args)
{
    Config config = loadConfig(args);
    const std::string password = resolvePassword(args, config);
    const std::string action = args.positional.empty() ? "" : args.positional[0];
    if (action == "build" || action == "init") {
        std::vector<std::filesystem::path> files;
        for (std::size_t i = 1; i < args.positional.size(); ++i)
            files.push_back(util::fromUtf8(args.positional[i]));
        const auto result = buildIntegrity(files, config.integrityFile(), password);
        std::cout << (result.ok ? "[OK] 完整性清单已生成" : "[X] 生成失败")
                  << "：文件 " << result.files << " 个\n";
        return result.ok ? 0 : 1;
    }
    if (action == "verify") {
        const auto result = verifyIntegrity(config.integrityFile(), password);
        std::cout << (result.ok ? "[OK] 完整性校验通过" : "[X] 完整性校验失败") << "：文件 "
                  << result.files << " 个";
        if (!result.ok)
            std::cout << "（" << result.firstProblem << "）";
        std::cout << "\n";
        return result.ok ? 0 : 5;
    }
    throw SecurityError(ErrorCode::InvalidArgument, "用法：integrity build <files...> | verify");
}

int cmdDoctor(const Args &args)
{
    Config config = loadConfig(args);
    int problems = 0;
    std::string checks;   // JSON 片段
    const auto addCheck = [&](const std::string &name, bool ok, const std::string &detail) {
        if (!checks.empty())
            checks += ",";
        checks += "{\"name\":\"" + util::jsonEscape(name) +
                  "\",\"ok\":" + (ok ? "true" : "false") +
                  ",\"detail\":\"" + util::jsonEscape(detail) + "\"}";
    };

    if (!args.json)
        std::cout << "安全模块自检\n";
    std::error_code error;
    std::filesystem::create_directories(config.home, error);
    const std::filesystem::path probe = config.home / ".write_probe";
    std::ofstream(probe) << "ok";
    const bool writable = std::filesystem::exists(probe, error);
    std::filesystem::remove(probe, error);
    if (!writable)
        ++problems;
    addCheck("home", writable, util::pathUtf8(config.home));
    if (!args.json)
        std::cout << "  [ " << (writable ? "OK" : "!!") << " ] 数据目录可写  "
                  << util::pathUtf8(config.home) << "\n";

    // 口令来源与其它命令一致（--password / FG_PASSWORD / dpapi.bin）；json 模式不会交互提问。
    std::string password;
    try {
        password = resolvePassword(args, config);
    } catch (const std::exception &) {
        password.clear();
    }
    if (!password.empty()) {
        try {
            audit::AuditLog log(config.auditDir(), password, config.logIterations);
            const auto verify = log.verify();
            if (!verify.ok)
                ++problems;
            addCheck("audit", verify.ok,
                     std::to_string(verify.records) + " 条记录");
            if (!args.json)
                std::cout << "  [ " << (verify.ok ? "OK" : "!!") << " ] 审计链  "
                          << verify.records << " 条\n";
        } catch (const std::exception &ex) {
            ++problems;
            addCheck("audit", false, ex.what());
            if (!args.json)
                std::cout << "  [ !! ] 审计链  " << ex.what() << "\n";
        }
        try {
            BackupStore store(config.backupDir(), password, config.iterations, config.logIterations);
            const auto verify = store.verify(false);
            if (!verify.ok)
                ++problems;
            addCheck("backup", verify.ok,
                     std::to_string(store.objectCount()) + " 个对象");
            if (!args.json)
                std::cout << "  [ " << (verify.ok ? "OK" : "!!") << " ] 备份索引  "
                          << store.objectCount() << " 个对象\n";
        } catch (const std::exception &ex) {
            ++problems;
            addCheck("backup", false, ex.what());
            if (!args.json)
                std::cout << "  [ !! ] 备份索引  " << ex.what() << "\n";
        }
    } else {
        addCheck("password", true, "未设置口令，审计/备份检查已跳过");
        if (!args.json)
            std::cout << "  [ -- ] 审计/备份：未设置口令，跳过\n";
    }

    if (args.json) {
        std::cout << "{\"ok\":" << (problems == 0 ? "true" : "false")
                  << ",\"checks\":[" << checks << "]}\n";
    } else {
        std::cout << (problems == 0 ? "自检通过" : "自检发现问题") << "\n";
    }
    return problems == 0 ? 0 : 5;
}

} // namespace

std::vector<std::string> wideArgsToUtf8()
{
    std::vector<std::string> result;
    int count = 0;
    LPWSTR *argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (!argv)
        return result;
    for (int i = 0; i < count; ++i) {
        const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(bytes > 0 ? bytes - 1 : 0), '\0');
        if (bytes > 1)
            ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, text.data(), bytes, nullptr, nullptr);
        result.push_back(text);
    }
    ::LocalFree(argv);
    return result;
}

int main()
{
    ::SetConsoleOutputCP(CP_UTF8);
    const std::vector<std::string> arguments = wideArgsToUtf8();
    if (arguments.size() < 2) {
        std::cout << "file_guard C++（安全模块）v1.0.0\n"
                     "用法：file_guard <命令> [参数]\n"
                     "  init [--no-dpapi] | doctor\n"
                     "  encrypt <文件|目录> [--dst] | decrypt <文件|目录> [--dst] [--force]\n"
                     "  verify <容器> | info <容器> | keygen [--dst] | passwd <容器> <新密码>\n"
                     "  scan <路径>\n"
                     "  log record|list|verify|head|export|clear\n"
                     "  backup add|list|restore|diff|purge|verify|gc [--keep N] [--dest-root] [--strip-prefix]\n"
                     "  vault create|add|list|extract|verify|recovery|passwd|info [--recovery-code]\n"
                     "  watch <路径...> [--auto-backup] [--auto-encrypt 秒]\n"
                     "  serve [--port] [--once] [--allow-remote] | integrity build|init|verify\n"
                     "全局：--home <目录> --password <密码> --keyfile <文件> --json --force --quiet\n";
        return 0;
    }

    Args args;
    try {
        const std::string command = arguments[1];
        args = parseArgs(arguments);

        if (command == "init") return cmdInit(args);
        if (command == "encrypt") return cmdEncrypt(args);
        if (command == "decrypt") return cmdDecrypt(args);
        if (command == "verify") return cmdVerify(args);
        if (command == "info") return cmdInfo(args);
        if (command == "scan") return cmdScan(args);
        if (command == "log") return cmdLog(args);
        if (command == "backup") return cmdBackup(args);
        if (command == "vault") return cmdVault(args);
        if (command == "passwd") return cmdPasswd(args);
        if (command == "keygen") return cmdKeygen(args);
        if (command == "watch") return cmdWatch(args);
        if (command == "serve") return cmdServe(args);
        if (command == "integrity") return cmdIntegrity(args);
        if (command == "doctor") return cmdDoctor(args);
        if (command == "version") {
            std::cout << "file_guard 1.0.0\n";
            return 0;
        }
        std::cerr << "未知命令：" << command << "\n";
        return 2;
    } catch (const SecurityError &error) {
        if (args.json) {
            std::cout << "{\"ok\":false,\"error\":{\"type\":\"" << errorTypeName(error.code())
                      << "\",\"message\":\"" << util::jsonEscape(error.what()) << "\"}}\n";
        } else {
            std::cerr << "错误：" << error.what() << "\n";
        }
        return static_cast<int>(error.code());
    } catch (const std::exception &error) {
        if (args.json) {
            std::cout << "{\"ok\":false,\"error\":{\"type\":\"Error\",\"message\":\""
                      << util::jsonEscape(error.what()) << "\"}}\n";
        } else {
            std::cerr << "错误：" << error.what() << "\n";
        }
        return 1;
    }
}
