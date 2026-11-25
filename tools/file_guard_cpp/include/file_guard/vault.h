#pragma once

// 保险箱：DEK + 恢复码 + 名称加密 + 口令轮换（C++ 移植）
//
// 布局：<dir>/vault.keys（key=value 密文块）、<dir>/containers/<随机hex>.fgc、
//       <dir>/manifest/（加密 JSONL 名称清单，复用审计链）。
// 说明：C++ 版密钥文件是 key=value（非 Python 的 JSON），不与 Python 版混用。

#include "file_guard/crypto.h"
#include "file_guard/util.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace file_guard {

struct VaultFileInfo {
    std::string name;
    std::string container;
    std::uint64_t size = 0;
    std::string sha256;
    std::string ts;
    std::string note;
    double mtime = 0;
};

class Vault {
public:
    Vault(std::filesystem::path directory, std::uint32_t iterations = crypto::kDefaultIterations,
          std::uint32_t logIterations = 200'000);

    bool exists() const;
    bool unlocked() const { return !m_dek.empty(); }
    const std::filesystem::path &directory() const { return m_directory; }

    struct CreateResult {
        std::string recoveryCode;
        bool dpapi = false;
    };
    CreateResult create(const std::string &password, bool useDpapi = true);

    void unlock(const std::string &password);
    void unlockWithRecovery(const std::string &recoveryCode);
    void unlockWithDpapi();
    void lock();

    std::vector<VaultFileInfo> list();

    VaultFileInfo addFile(const std::filesystem::path &source, const std::string &note = {},
                          bool removeSource = false);

    struct ExtractResult {
        std::string name;
        std::string restoredTo;
        std::string sha256;
        std::uint64_t size = 0;
    };
    ExtractResult extract(const std::string &target, std::filesystem::path destDir = {},
                          bool overwrite = false);

    void changePassword(const std::string &oldPassword, const std::string &newPassword);
    std::string recoveryCode(const std::string &password);

    struct VerifyResult {
        bool ok = false;
        std::size_t files = 0;
        std::size_t problems = 0;
        std::string firstProblem;
    };
    VerifyResult verify();

    static std::string normalizeRecoveryCode(const std::string &text);
    static std::string formatRecoveryCode(const std::string &normalized);

private:
    struct Keys;
    Keys loadKeys() const;
    void saveKeys(const Keys &keys) const;
    std::string requireDek() const;
    std::string manifestPassword() const;
    std::vector<std::string> manifestRecords();
    std::filesystem::path safeContainer(const std::string &relative) const;

    std::filesystem::path m_directory;
    std::uint32_t m_iterations;
    std::uint32_t m_logIterations;
    std::string m_dek; // 十六进制编码的 32 字节 DEK（空 = 未解锁）
};

} // namespace file_guard
