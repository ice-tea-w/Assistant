#include "file_guard/config.h"

#include <windows.h>

#include <cstdlib>

namespace file_guard {

Config Config::load(const std::filesystem::path &home)
{
    Config config;
    config.home = home.empty() ? util::defaultHome() : home;

    const std::string text = util::readText(config.home / "config.txt");
    for (const std::string &line : util::splitLines(text)) {
        const std::string trimmed = util::trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        const std::size_t equal = trimmed.find('=');
        if (equal == std::string::npos)
            continue;
        const std::string key = util::trim(trimmed.substr(0, equal));
        const std::string value = util::trim(trimmed.substr(equal + 1));
        if (key == "iterations")
            config.iterations = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "chunk_size")
            config.chunkSize = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "log_iterations")
            config.logIterations = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "port")
            config.port = std::atoi(value.c_str());
    }
    return config;
}

void Config::save() const
{
    util::writeTextAtomic(
        home / "config.txt",
        "# file_guard C++ 配置（key=value）\niterations=" + std::to_string(iterations) +
            "\nchunk_size=" + std::to_string(chunkSize) +
            "\nlog_iterations=" + std::to_string(logIterations) +
            "\nport=" + std::to_string(port) + "\n");
}

std::filesystem::path Config::path(const std::string &name) const
{
    return home / name;
}

std::filesystem::path Config::auditDir() const { return home / "audit"; }
std::filesystem::path Config::backupDir() const { return home / "backups"; }
std::filesystem::path Config::vaultDir() const { return home / "vault"; }
std::filesystem::path Config::integrityFile() const { return home / "integrity.txt"; }
std::filesystem::path Config::tokenFile() const { return home / "service.token"; }
std::filesystem::path Config::dpapiFile() const { return home / "dpapi.bin"; }

void Config::ensureDirs() const
{
    std::error_code ignored;
    std::filesystem::create_directories(home, ignored);
    std::filesystem::create_directories(auditDir(), ignored);
    std::filesystem::create_directories(backupDir(), ignored);
    std::filesystem::create_directories(vaultDir(), ignored);
}

} // namespace file_guard
