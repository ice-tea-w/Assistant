#include "file_guard/watch.h"

#include "file_guard/crypto.h"
#include "file_guard/util.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace file_guard {

namespace {

bool isReparse(const std::filesystem::path &path)
{
    const DWORD attrs = ::GetFileAttributesW(path.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return true;
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool shouldSkip(const std::filesystem::path &path)
{
    const std::string name = util::pathUtf8(path.filename());
    if (name.empty() || name == "." || name == "..")
        return true;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".fgc") == 0)
        return true;
    if (name.find(".part-") != std::string::npos || name.size() > 4 &&
            (name.compare(name.size() - 4, 4, ".tmp") == 0))
        return true;
    if (!name.empty() && name[0] == '.')
        return true;
    return isReparse(path);
}

} // namespace

DirectoryWatcher::DirectoryWatcher(std::filesystem::path directory, bool recursive)
    : m_directory(std::move(directory))
    , m_recursive(recursive)
{
}

DirectoryWatcher::~DirectoryWatcher()
{
    stop();
}

bool DirectoryWatcher::start(std::function<void(const WatchEvent &)> handler, std::string *error)
{
    if (m_running.load())
        return true;
    if (!std::filesystem::is_directory(m_directory)) {
        if (error)
            *error = "目录不存在：" + util::pathUtf8(m_directory);
        return false;
    }
    m_handler = std::move(handler);
    m_running.store(true);
    m_thread = std::thread([this]() { loop(); });
    return true;
}

void DirectoryWatcher::stop()
{
    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();
}

void DirectoryWatcher::loop()
{
    const HANDLE handle = CreateFileW(
        m_directory.wstring().c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    std::vector<std::uint8_t> buffer(64 * 1024);
    while (m_running.load()) {
        DWORD returned = 0;
        const BOOL ok = ReadDirectoryChangesW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()), m_recursive,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &returned, nullptr, nullptr);
        if (!ok) {
            Sleep(200);
            continue;
        }
        if (returned == 0)
            continue;

        std::size_t offset = 0;
        while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= returned) {
            const auto *info =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(buffer.data() + offset);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            WatchEvent event;
            event.path = m_directory / name;
            switch (info->Action) {
            case FILE_ACTION_ADDED:
            case FILE_ACTION_RENAMED_NEW_NAME:
                event.kind = WatchKind::Created;
                break;
            case FILE_ACTION_REMOVED:
            case FILE_ACTION_RENAMED_OLD_NAME:
                event.kind = WatchKind::Removed;
                break;
            default:
                event.kind = WatchKind::Modified;
                break;
            }
            if (event.kind != WatchKind::Removed && m_handler && !shouldSkip(event.path))
                m_handler(event);
            if (info->NextEntryOffset == 0)
                break;
            offset += info->NextEntryOffset;
        }
    }
    CloseHandle(handle);
}

std::vector<std::filesystem::path> collectFiles(const std::filesystem::path &root, bool recursive)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::exists(root, error))
        return files;
    if (std::filesystem::is_regular_file(root, error)) {
        if (!shouldSkip(root))
            files.push_back(root);
        return files;
    }
    if (!std::filesystem::is_directory(root, error))
        return files;

    if (recursive) {
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, error);
        for (const auto end = std::filesystem::recursive_directory_iterator(); it != end;
             it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (isReparse(it->path())) {
                if (it->is_directory(error))
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(error) && !shouldSkip(it->path()))
                files.push_back(it->path());
        }
    } else {
        for (const auto &entry : std::filesystem::directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, error)) {
            if (entry.is_regular_file(error) && !shouldSkip(entry.path()))
                files.push_back(entry.path());
        }
    }
    return files;
}

std::vector<AutoEncryptResult> autoEncryptIdle(const std::vector<std::filesystem::path> &roots,
                                               const AutoEncryptOptions &options)
{
    std::vector<AutoEncryptResult> results;
    const auto now = std::filesystem::file_time_type::clock::now();

    for (const auto &root : roots) {
        for (const auto &path : collectFiles(root, true)) {
            std::error_code error;
            const auto mtime = std::filesystem::last_write_time(path, error);
            if (error)
                continue;
            const double idle =
                std::chrono::duration<double>(now - mtime).count();
            if (idle < options.idleSeconds)
                continue;

            AutoEncryptResult result;
            result.path = path;
            result.mtime = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count());
            if (options.dryRun) {
                result.action = "would-encrypt";
                results.push_back(result);
                continue;
            }

            const std::filesystem::path dst = std::filesystem::path(path.wstring() + L".fgc");
            if (std::filesystem::exists(dst)) {
                result.action = "skipped";
                result.error = "容器已存在";
                results.push_back(result);
                continue;
            }
            try {
                const crypto::Result encrypted =
                    crypto::encryptFile(path, dst, options.password, options.iterations,
                                        options.chunkSize);
                result.dst = util::pathUtf8(dst);
                if (options.removeSource) {
                    const crypto::Result check =
                        crypto::verifyFile(dst, options.password);
                    if (!crypto::constantTimeEquals(check.sha256, encrypted.sha256)) {
                        result.action = "error";
                        result.error = "加密结果校验失败，已保留明文";
                        results.push_back(result);
                        continue;
                    }
                    std::filesystem::remove(path, error);
                    if (error) {
                        result.action = "error";
                        result.error = "删除明文失败：" + error.message();
                        results.push_back(result);
                        continue;
                    }
                }
                result.action = "encrypted";
            } catch (const std::exception &ex) {
                result.action = "error";
                result.error = ex.what();
            }
            results.push_back(result);
        }
    }
    return results;
}

} // namespace file_guard
