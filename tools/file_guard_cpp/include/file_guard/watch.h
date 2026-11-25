#pragma once

// 目录监听 + 空闲自动加密（C++ 移植 watch.py）
//
// * DirectoryWatcher：ReadDirectoryChangesW 阻塞线程；
// * collectFiles：遍历（跳过 .fgc/隐藏/重解析点），供自动加密与备份复用；
// * autoEncryptIdle：把"空闲超过 N 秒"的文件加密，verify 通过后才删明文。

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace file_guard {

enum class WatchKind { Created, Modified, Removed };

struct WatchEvent {
    std::filesystem::path path;
    WatchKind kind = WatchKind::Modified;
};

class DirectoryWatcher {
public:
    DirectoryWatcher(std::filesystem::path directory, bool recursive = true);
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher &) = delete;
    DirectoryWatcher &operator=(const DirectoryWatcher &) = delete;

    bool start(std::function<void(const WatchEvent &)> handler, std::string *error);
    void stop();
    bool running() const { return m_running.load(); }

private:
    void loop();

    std::filesystem::path m_directory;
    bool m_recursive = true;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::function<void(const WatchEvent &)> m_handler;
    void *m_handle = nullptr;
};

/// 遍历 root（文件或目录），跳过 .fgc / 隐藏文件 / 重解析点。
std::vector<std::filesystem::path> collectFiles(const std::filesystem::path &root,
                                                bool recursive = true);

struct AutoEncryptOptions {
    std::string password;
    double idleSeconds = 60.0;
    std::uint32_t iterations = 600'000;
    std::uint32_t chunkSize = 1u << 20;
    bool removeSource = true;
    bool dryRun = false;
};

struct AutoEncryptResult {
    std::filesystem::path path;
    std::string action;   ///< encrypted / would-encrypt / skipped / error
    std::string dst;
    std::string error;
    std::uint64_t mtime = 0;
};

std::vector<AutoEncryptResult> autoEncryptIdle(const std::vector<std::filesystem::path> &roots,
                                               const AutoEncryptOptions &options);

} // namespace file_guard
