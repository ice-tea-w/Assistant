#pragma once

#include "file_guard/crypto.h"
#include "file_guard/util.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace file_guard {

struct Config {
    std::filesystem::path home;
    std::uint32_t iterations = crypto::kDefaultIterations;
    std::uint32_t chunkSize = crypto::kDefaultChunkSize;
    std::uint32_t logIterations = 200'000;
    int port = 8765;

    static Config load(const std::filesystem::path &home = {});
    void save() const;

    std::filesystem::path path(const std::string &name) const;
    std::filesystem::path auditDir() const;
    std::filesystem::path backupDir() const;
    std::filesystem::path vaultDir() const;
    std::filesystem::path integrityFile() const;
    std::filesystem::path tokenFile() const;
    std::filesystem::path dpapiFile() const;

    void ensureDirs() const;
};

} // namespace file_guard
