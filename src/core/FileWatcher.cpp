#include "core/FileWatcher.h"

#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

QString normaliseRoot(const QString &root)
{
    QString r = QDir::cleanPath(root);
    while (r.size() > 3 && r.endsWith('/'))
        r.chop(1);
    return r;
}

QString joinPath(const QString &root, const QString &relative)
{
    if (relative.isEmpty())
        return root;
    QString rel = relative;
    rel.replace('\\', '/');
    return QDir::cleanPath(root + "/" + rel);
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME
                          | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;

/// 一个根的监听状态。用 OVERLAPPED + 事件对象，好处是**第一次 ReadDirectoryChangesW
/// 在 start() 里就发出去了**，`start()` 返回时监听已经生效，调用方紧接着的操作不会
/// 掉进"线程还没跑到第一次 read"的窗口里。
struct RootWatch
{
    QString root;
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED ov{};
    std::vector<BYTE> buffer;
    bool armed = false;
};

bool issueRead(RootWatch *rw)
{
    rw->buffer.assign(64 * 1024, 0);
    ZeroMemory(&rw->ov, sizeof(rw->ov));
    rw->ov.hEvent = rw->event;
    ResetEvent(rw->event);

    const BOOL ok = ReadDirectoryChangesW(rw->dir, rw->buffer.data(),
                                          static_cast<DWORD>(rw->buffer.size()),
                                          TRUE, kFilter, nullptr, &rw->ov, nullptr);
    if (ok) {
        rw->armed = true;
        return true;
    }
    const DWORD err = GetLastError();
    rw->armed = (err == ERROR_IO_PENDING);
    return rw->armed;
}

} // namespace

struct FileWatcher::Impl
{
    std::atomic<bool> running{false};
    int coalesceMs = 100;
    QStringList roots;
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<RootWatch>> watches;

    std::mutex mutex;                              ///< 保护 pending + overflow
    std::map<QString, FileSystemEvent> pending;    ///< key: 小写路径
    QVector<FileSystemEvent> overflow;

    QTimer *timer = nullptr;
    FileWatcher *owner = nullptr;

    ~Impl() { stopAll(); }

    void push(const FileSystemEvent &ev)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (ev.type == FileSystemEvent::Overflow) {
            overflow.push_back(ev);
            return;
        }
        // 同路径合并：只保留"对消费者最有意义"的那一条。
        // 典型场景：新建文件 = ADDED + MODIFIED（写内容），若简单"最后一条获胜"，
        // 消费者只会看到 Modified，而索引里根本没有这个文件。
        const QString key = ev.path.toLower();
        auto it = pending.find(key);
        if (it == pending.end()) {
            pending[key] = ev;
            return;
        }

        FileSystemEvent &cur = it->second;
        const auto from = cur.type;
        const auto to = ev.type;
        if (from == to) {
            cur = ev;
        } else if (from == FileSystemEvent::Added && to == FileSystemEvent::Modified) {
            // 新建后立刻写入：仍然是"新增"
        } else if (from == FileSystemEvent::Added && to == FileSystemEvent::Removed) {
            pending.erase(it);              // 窗口内建了又删，等于没发生
        } else if (from == FileSystemEvent::Removed && to == FileSystemEvent::Added) {
            cur = ev;
            cur.type = FileSystemEvent::Modified;   // 同名替换
            cur.oldPath.clear();
        } else if (from == FileSystemEvent::Renamed && to == FileSystemEvent::Modified) {
            // 改名后写入：保留旧路径信息
            cur.timestamp = ev.timestamp;
        } else {
            cur = ev;
        }
    }

    void flush()
    {
        QVector<FileSystemEvent> batch;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pending.empty() && overflow.isEmpty())
                return;
            batch.reserve(static_cast<int>(pending.size() + overflow.size()));
            for (auto &kv : pending)
                batch.push_back(kv.second);
            pending.clear();
            batch += overflow;
            overflow.clear();
        }
        emit owner->batchReady(batch);
    }

    void stopAll()
    {
        if (!running.exchange(false))
            return;

        for (auto &w : watches) {
            if (w->dir != INVALID_HANDLE_VALUE) {
                CancelIoEx(w->dir, nullptr);   // 唤醒阻塞中的 GetOverlappedResult
                CancelIoEx(w->dir, &w->ov);
            }
        }

        for (auto &t : threads) {
            if (t.joinable())
                t.join();
        }
        threads.clear();

        for (auto &w : watches) {
            if (w->event)
                CloseHandle(w->event);
            if (w->dir != INVALID_HANDLE_VALUE)
                CloseHandle(w->dir);
        }
        watches.clear();

        if (timer)
            timer->stop();
        flush();   // 停之前把已经收到的发出去
    }
};

namespace {

void parseBatch(FileWatcher::Impl *d, const QString &root, const BYTE *data, DWORD bytes)
{
    QString pendingOldName;
    auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(data);
    const BYTE *end = data + bytes;

    for (;;) {
        const int chars = static_cast<int>(info->FileNameLength / sizeof(wchar_t));
        const QString name = QString::fromWCharArray(info->FileName, chars);
        const QString full = joinPath(root, name);

        FileSystemEvent ev;
        ev.path = full;
        ev.timestamp = nowMs();

        switch (info->Action) {
        case FILE_ACTION_ADDED:
            ev.type = FileSystemEvent::Added;
            d->push(ev);
            break;
        case FILE_ACTION_REMOVED:
            ev.type = FileSystemEvent::Removed;
            d->push(ev);
            break;
        case FILE_ACTION_MODIFIED:
            ev.type = FileSystemEvent::Modified;
            d->push(ev);
            break;
        case FILE_ACTION_RENAMED_OLD_NAME:
            pendingOldName = full;          // 与紧随其后的 NEW_NAME 配对
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            ev.type = FileSystemEvent::Renamed;
            ev.oldPath = pendingOldName;
            pendingOldName.clear();
            d->push(ev);
            break;
        default:
            break;
        }

        if (info->NextEntryOffset == 0)
            break;
        const BYTE *next = reinterpret_cast<const BYTE *>(info) + info->NextEntryOffset;
        if (next >= end)
            break;
        info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(next);
    }
}

void watchLoop(FileWatcher::Impl *d, RootWatch *rw)
{
    while (d->running.load()) {
        const DWORD wait = WaitForSingleObject(rw->event, 200);
        if (wait == WAIT_TIMEOUT)
            continue;
        if (wait != WAIT_OBJECT_0)
            break;
        if (!d->running.load())
            break;

        DWORD bytes = 0;
        const BOOL ok = GetOverlappedResult(rw->dir, &rw->ov, &bytes, FALSE);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED)
                break;                       // stop() 主动取消

            FileSystemEvent ev;
            ev.type = FileSystemEvent::Overflow;   // 溢出/根目录失效：让消费者重扫
            ev.path = rw->root;
            ev.timestamp = nowMs();
            d->push(ev);
            Logger::get().warn("FileWatcher: {} 读事件失败 (error {})，发 Overflow",
                               rw->root.toStdString(), static_cast<int>(err));
            if (!issueRead(rw))
                break;
            continue;
        }

        if (bytes > 0)
            parseBatch(d, rw->root, rw->buffer.data(), bytes);
        else {
            FileSystemEvent ev;
            ev.type = FileSystemEvent::Overflow;
            ev.path = rw->root;
            ev.timestamp = nowMs();
            d->push(ev);
            Logger::get().warn("FileWatcher: {} 事件缓冲区溢出，发 Overflow",
                               rw->root.toStdString());
        }

        if (!issueRead(rw))
            break;
    }
}

} // namespace

FileWatcher::FileWatcher(QObject *parent)
    : QObject(parent)
    , d(new Impl)
{
    d->owner = this;
    qRegisterMetaType<QVector<FileSystemEvent>>("QVector<FileSystemEvent>");
}

FileWatcher::~FileWatcher()
{
    stop();
}

bool FileWatcher::start(const QStringList &roots)
{
    stop();

    QStringList accepted;
    for (const QString &raw : roots) {
        const QString root = normaliseRoot(raw);
        if (!QFileInfo(root).isDir())
            continue;
        if (!accepted.contains(root, Qt::CaseInsensitive))
            accepted.push_back(root);
    }
    if (accepted.isEmpty())
        return false;

    d->roots = accepted;

    if (!d->timer) {
        d->timer = new QTimer(this);
        d->timer->setTimerType(Qt::CoarseTimer);
        connect(d->timer, &QTimer::timeout, this, [this] { d->flush(); });
    }

    // 打开句柄并把第一次 read 发出去；start() 返回即表示监听已生效。
    for (const QString &root : accepted) {
        auto rw = std::make_unique<RootWatch>();
        rw->root = root;
        rw->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        rw->dir = CreateFileW(QDir::toNativeSeparators(root).toStdWString().c_str(),
                              FILE_LIST_DIRECTORY,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                              nullptr);
        if (rw->dir == INVALID_HANDLE_VALUE) {
            Logger::get().warn("FileWatcher: 无法打开 {} (error {})",
                               root.toStdString(), static_cast<int>(GetLastError()));
            if (rw->event)
                CloseHandle(rw->event);
            continue;
        }
        if (!issueRead(rw.get())) {
            Logger::get().warn("FileWatcher: {} 首次 read 失败 (error {})",
                               root.toStdString(), static_cast<int>(GetLastError()));
            CloseHandle(rw->event);
            CloseHandle(rw->dir);
            continue;
        }
        d->watches.push_back(std::move(rw));
    }

    if (d->watches.empty())
        return false;

    d->running.store(true);
    d->timer->start(qMax(1, d->coalesceMs));

    for (auto &w : d->watches) {
        RootWatch *raw = w.get();
        d->threads.emplace_back([this, raw] { watchLoop(d.get(), raw); });
    }

    Logger::get().info("FileWatcher: watching {} root(s), coalesce {} ms",
                       d->watches.size(), d->coalesceMs);
    return true;
}

void FileWatcher::stop()
{
    d->stopAll();
}

bool FileWatcher::isRunning() const
{
    return d->running.load();
}

QStringList FileWatcher::roots() const
{
    return d->roots;
}

void FileWatcher::setCoalesceMs(int ms)
{
    d->coalesceMs = qMax(0, ms);
    if (d->timer && d->timer->isActive())
        d->timer->start(qMax(1, d->coalesceMs));
}

int FileWatcher::coalesceMs() const
{
    return d->coalesceMs;
}
