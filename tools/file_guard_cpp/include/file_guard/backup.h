#pragma once

// 备份：内容寻址加密对象 + 加密索引 + 版本回滚 + 冲突保护（C++ 移植）

#include "file_guard/crypto.h"
#include "file_guard/util.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace file_guard {

struct BackupEntry {
    std::string path;
    std::uint32_t version = 0;
    std::string object;
    std::string sha256;
    std::uint64_t size = 0;
    double mtime = 0;
    std::string ts;
};

class BackupStore {
public:
    BackupStore(std::filesystem::path directory, std::string password,
                std::uint32_t iterations = crypto::kDefaultIterations,
                std::uint32_t logIterations = 200'000);

    const std::filesystem::path &directory() const { return m_directory; }

    BackupEntry backupFile(const std::filesystem::path &source);
    std::vector<BackupEntry> list();
    std::vector<BackupEntry> versions(const std::string &path);

    struct RestoreResult {
        std::string path;
        std::string restoredTo;
        std::string sha256;
        std::uint32_t version = 0;
        bool conflict = false;
    };
    /// version=0 表示最新版本；dest 为空则还原到原路径。
    /// destRoot/stripPrefix 用于跨机/异地还原（把旧绝对路径映射到新根目录）。
    RestoreResult restore(const std::string &path, std::uint32_t version = 0,
                          std::filesystem::path dest = {}, bool force = false,
                          std::filesystem::path destRoot = {},
                          std::filesystem::path stripPrefix = {});

    struct DiffResult {
        std::string path;
        std::uint32_t fromVersion = 0;
        std::uint32_t toVersion = 0;
        bool changed = false;
        std::string kind;       ///< "none" / "text" / "binary"
        std::string text;       ///< unified diff（最多 maxLines）
        bool truncated = false;
        std::string fromSha;
        std::string toSha;
        std::uint64_t fromSize = 0;
        std::uint64_t toSize = 0;
    };
    /// 比较同一路径两个版本（默认上一版 vs 最新版）。
    DiffResult diff(const std::string &path, std::uint32_t fromVersion = 0,
                    std::uint32_t toVersion = 0, std::size_t maxBytes = 1u << 20,
                    std::size_t maxLines = 400);

    /// 每个路径只保留最近 keep 个版本（其余标记 purge，gc 会删除对象）。
    std::size_t purge(std::uint32_t keep);

    struct VerifyResult {
        bool ok = false;
        std::size_t objects = 0;
        std::size_t problems = 0;
        std::string firstProblem;
    };
    VerifyResult verify(bool full = true);

    std::size_t gc();
    std::size_t objectCount() const;
    std::filesystem::path objectPath(const std::string &object) const;

private:
    std::string objectNameFor(const std::filesystem::path &source) const;
    std::string objectPassword() const;
    std::vector<BackupEntry> readIndex();

    std::filesystem::path m_directory;
    std::string m_password;
    std::uint32_t m_iterations;
    std::uint32_t m_logIterations;
};

} // namespace file_guard
