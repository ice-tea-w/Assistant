#pragma once

// 模块完整性自校验：文件 SHA-256 清单 + HMAC 签名。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace file_guard {

struct IntegrityReport {
    bool ok = false;
    std::size_t files = 0;
    std::size_t problems = 0;
    std::string firstProblem;
};

IntegrityReport buildIntegrity(const std::vector<std::filesystem::path> &files,
                               const std::filesystem::path &manifest,
                               const std::string &password,
                               std::uint32_t iterations = 20'000);

IntegrityReport verifyIntegrity(const std::filesystem::path &manifest,
                                const std::string &password,
                                std::uint32_t iterations = 20'000);

} // namespace file_guard
