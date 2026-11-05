#include "core/FileIndex.h"

#include "core/FileEventDispatcher.h"
#include "core/FileSnapshot.h"
#include "core/SnapshotManager.h"
#include "core/FileScanner.h"
#include "core/QueryEngine.h"
#include "core/Logger.h"
#include "database/FileIndexDatabase.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

namespace {

/// §3.3 的两档阈值（**绝不静默驱逐**）：
/// - 软预算：超了**继续索引**，只在日志里说明实际规模与"怎么缩小"；
/// - 硬保护：超了**停止索引**（不驱逐已有条目）并醒目告警。
/// 控制规模的手段是"少收"（roots / 排除规则 / 种类白名单），不是"事后丢"。
constexpr int kSoftBudgetEntries = 300000;
constexpr int kHardLimitEntries  = 1000000;

} // namespace

/// 扫描工作线程：采集（`FileScanner`）+ 差量写库（`FileIndexDatabase`）+ 建快照。
///
/// 三件事都放在这里，是因为 `QSqlDatabase` 的连接**线程亲和**：连接在 `run()` 里
/// 建立、在同一个 `run()` 里销毁，所以差量写库不会卡住 GUI 线程（§3.4）。
/// 门面只在 `finished` 之后收结果 —— 那时 `run()` 已返回，两者之间的先后有
/// Qt 的排队投递兜底。
class ScanWorker : public QThread
{
public:
    ScanWorker(int entryLimit, QString databasePath, QStringList roots,
               QObject *parent = nullptr)
        : QThread(parent)
        , m_entryLimit(entryLimit > 0 ? entryLimit : kHardLimitEntries)
        , m_databasePath(std::move(databasePath))
        , m_roots(std::move(roots))
    {
    }

    /// 请求取消（析构/退出时用，避免"退出要等扫描跑完"）。
    void cancel() { m_cancel.store(true, std::memory_order_relaxed); }

    // ---- run() 写，GUI 线程在 finished 之后读 ----
    std::shared_ptr<FileSnapshot> snapshot;
    qint64                    elapsedMs = 0;
    FileScanner::Stats        scan;
    FileIndexDatabase::Diff   diff;
    bool                      dbOpened = false;
    int                       dbRows   = 0;
    QString                   dbError;

protected:
    void run() override
    {
        QElapsedTimer timer;
        timer.start();

        // ---- 1. 采集（worker 线程，可取消） ----
        FileScanner::Options options;
        options.maxEntries       = m_entryLimit;
        options.excludedDirNames = FileScanner::defaultExcludedDirNames();

        const QStringList roots = m_roots.isEmpty() ? FileScanner::defaultRoots() : m_roots;
        QVector<FileScanner::Entry> entries;
        scan = FileScanner::scan(roots, options, &entries,
                                 [this]() {
                                     return m_cancel.load(std::memory_order_relaxed);
                                 });

        Logger::get().info("FileScanner: {} files / {} dirs in {} ms "
                           "(pruned {}, denied {}, cancelled={}, hitLimit={})",
                           scan.files, scan.dirsVisited, scan.elapsedMs,
                           scan.prunedDirs, scan.deniedDirs,
                           scan.cancelled, scan.hitHardLimit);

        // ---- 2. 差量持久化（同一线程：QSqlDatabase 线程亲和） ----
        if (!m_databasePath.isEmpty()) {
            FileIndexDatabase db;                    // 连接在本线程建立、本线程销毁
            if (db.open(m_databasePath)) {
                dbOpened = true;

                QVector<FileIndexDatabase::FileRow> diskRows;
                diskRows.reserve(entries.size());
                for (const FileScanner::Entry &e : entries) {
                    FileIndexDatabase::FileRow row;
                    row.path  = e.path();
                    row.name  = e.name;
                    row.size  = e.size;
                    row.mtime = e.mtime;
                    diskRows.append(std::move(row));
                }

                diff = FileIndexDatabase::diff(diskRows, db.loadAll());

                if (!diff.empty() && !db.applyDiff(diff))
                    dbError = db.lastError();

                const qint64 now = QDateTime::currentSecsSinceEpoch();
                for (const QString &root : roots) {
                    if (!root.isEmpty())
                        db.touchRoot(root, now);
                }

                dbRows = db.rowCount();
                db.close();

                Logger::get().info("FileIndexDatabase: +{} / ~{} / -{} / ={}（库内 {} 行）",
                                   diff.added.size(), diff.changed.size(),
                                   diff.removed.size(), diff.unchanged, dbRows);
            } else {
                dbError = db.lastError();
                Logger::get().warn("FileIndexDatabase: 跳过持久化 — {}",
                                   dbError.toStdString());
            }
        }

        // ---- 3. 建快照（不可变；发布由 GUI 线程做） ----
        FileSnapshot::Builder builder;
        builder.reserve(entries.size());
        for (const FileScanner::Entry &e : entries)
            builder.add(e.name, e.path(), e.size, e.mtime);

        snapshot  = builder.finish();
        elapsedMs = timer.elapsed();

        if (scan.hitHardLimit)
            Logger::get().warn("FileIndex: 已达硬保护上限 {} 条，停止索引（**不驱逐**已有条目）；"
                               "建议收窄 roots 或补排除规则", m_entryLimit);
        else if (scan.files >= kSoftBudgetEntries)
            Logger::get().warn("FileIndex: 条目数 {} 已超软预算 {}（继续索引，仅提示）",
                               scan.files, kSoftBudgetEntries);
    }

private:
    std::atomic<bool> m_cancel{false};
    const int         m_entryLimit;
    const QString     m_databasePath;
    const QStringList m_roots;   ///< 空 = FileScanner::defaultRoots()
};

// ---------------------------------------------------------------------------
// FileIndex
// ---------------------------------------------------------------------------

FileIndex::FileIndex(QObject *parent)
    : QObject(parent)
    , m_snapshots(std::make_unique<SnapshotManager>())
    , m_query(std::make_unique<QueryEngine>(*m_snapshots))
{
}

FileIndex::~FileIndex()
{
    // 不能带着还在跑的线程析构：先请它收手，再等。扫描在"每取一个条目"处检查
    // 取消标志，所以这里最多等一条目录项的处理时间。
    if (m_worker) {
        m_worker->cancel();
        m_worker->wait();
    }
}

void FileIndex::setDatabasePath(const QString &path)
{
    m_databasePath = path;
}

void FileIndex::startAsync()
{
    if (m_scanning) {
        // 已在扫：记一个待重扫，等这轮结束再起，避免两个 worker 抢着发布
        m_pendingRefresh = true;
        return;
    }

    // A6：冷启动先走 DB —— 有库且当前没有快照时，先把"上次看到的世界"发布出去，
    // 界面立刻可查；随后再扫描并校正。
    if (!m_snapshots->current() && !m_databasePath.isEmpty()
        && QFileInfo::exists(m_databasePath)) {
        startColdLoad();
        return;
    }

    startScan();
}

void FileIndex::startColdLoad()
{
    // 同步载入：32k 行约 20–40 ms，换来的是"没有第二个线程生命周期要管"。
    // 200k 行的量级需要改成异步（A6b），届时用与 ScanWorker 同样的
    // "线程对象由 FileIndex 持有、析构里 wait" 的方式，而不是 QThread::create。
    QElapsedTimer timer;
    timer.start();

    m_scanning = true;
    emit indexStateChanged();

    FileSnapshot::Builder builder;
    int rows = 0;
    {
        FileIndexDatabase db(QStringLiteral("file_index_cold"));
        if (db.open(m_databasePath)) {
            const auto all = db.loadAll();
            rows = static_cast<int>(all.size());
            builder.reserve(rows);
            for (const auto &row : all)
                builder.add(row.name, row.path, row.size, row.mtime);
            db.close();
        }
    }

    auto snapshot = builder.finish();
    m_dbRows    = rows;
    m_coldMs    = timer.elapsed();
    m_fromCache = true;
    m_scanning  = false;

    m_snapshots->publish(snapshot);
    const int entries = snapshot->size();

    Logger::get().info("FileIndex: 冷启动从 DB 载入 {} 条（{} ms），立即可查；随后后台校正",
                       entries, m_coldMs);
    emit cacheLoaded(entries, m_coldMs);
    emit indexStateChanged();

    startScan();                   // 校正
}

void FileIndex::refresh()
{
    startAsync();
}

void FileIndex::startScan()
{
    if (m_worker) {
        // 上一轮的 finished 信号早已发完（m_scanning 为假才会走到这里），
        // 所以现在回收是安全的；不能在下面的槽里删，那是它自己在发信号。
        m_worker->wait();
        m_worker.reset();
    }

    m_worker = std::make_unique<ScanWorker>(m_entryLimit, m_databasePath, m_roots);
    connect(m_worker.get(), &QThread::finished, this, &FileIndex::onScanFinished);

    m_scanning = true;
    emit indexStateChanged();
    m_worker->start();
}

void FileIndex::onScanFinished()
{
    m_scanning = false;

    if (m_worker) {
        m_worker->wait();

        std::shared_ptr<const FileSnapshot> snap = m_worker->snapshot;
        m_msLastScan = m_worker->elapsedMs;
        m_dbRows     = m_worker->dbRows;

        const int entries = snap ? snap->size() : 0;
        m_fromCache = false;
        m_snapshots->publish(std::move(snap));

        Logger::get().info("FileIndex: published snapshot #{} — {} files in {} ms (db {} rows)",
                           m_snapshots->generation(), entries, m_msLastScan, m_dbRows);

        emit indexStateChanged();
        emit indexReady(entries, m_msLastScan);
    }

    if (m_pendingRefresh) {
        m_pendingRefresh = false;
        startScan();
    }
}

void FileIndex::setSnapshot(std::shared_ptr<const FileSnapshot> snapshot)
{
    m_snapshots->publish(std::move(snapshot));
    emit indexStateChanged();
}

void FileIndex::setEntryLimit(int maxEntries)
{
    m_entryLimit = maxEntries > 0 ? maxEntries : 0;
}

QList<SearchResult> FileIndex::query(const QString &text, int limit)
{
    QList<SearchResult> out;

    // 门面在这里只剩"转发 + 格式转换"：查询逻辑全在 QueryEngine（Parser → 打分 → top-K），
    // 快照的只读接口把物理表示挡在后面。A4 之后这个函数不该再长出别的分支。
    const std::shared_ptr<const FileSnapshot> snap = m_snapshots->current();
    if (!snap || limit <= 0)
        return out;

    const QList<QueryEngine::Match> matches = m_query->query(text, limit);
    out.reserve(matches.size());
    for (const QueryEngine::Match &m : matches) {
        const QString path = snap->pathAt(m.index);

        SearchResult item;
        item.id       = QStringLiteral("file://") + path;
        item.title    = snap->nameAt(m.index);
        item.subtitle = path;
        item.type     = SearchResult::File;
        item.priority = 1;              // 与内建/应用同分时排在后面
        item.score    = m.score;
        item.path     = path;
        out.append(item);
    }
    return out;
}

FileIndex::IndexStats FileIndex::stats() const
{
    IndexStats s;
    const std::shared_ptr<const FileSnapshot> snap = m_snapshots->current();
    s.entries    = snap ? snap->size() : 0;
    s.dbRows     = m_dbRows;
    s.ready      = static_cast<bool>(snap);
    s.scanning   = m_scanning;
    s.msLastScan = m_msLastScan;
    s.generation = m_snapshots->generation();
    s.fromCache  = m_fromCache;
    return s;
}

bool FileIndex::isReady() const
{
    return static_cast<bool>(m_snapshots->current());
}

// ---------------------------------------------------------------------------
// A5b：增量补丁
// ---------------------------------------------------------------------------

void FileIndex::setRoots(const QStringList &roots)
{
    m_roots = roots;
}

QStringList FileIndex::roots() const
{
    return m_roots.isEmpty() ? FileScanner::defaultRoots() : m_roots;
}

void FileIndex::setEventSource(FileEventDispatcher *dispatcher)
{
    if (m_dispatcher && m_subscription > 0)
        m_dispatcher->unsubscribe(m_subscription);

    m_dispatcher = dispatcher;
    m_subscription = 0;
    if (!m_dispatcher)
        return;

    if (!m_eventTimer) {
        m_eventTimer = new QTimer(this);
        m_eventTimer->setSingleShot(true);
        m_eventTimer->setInterval(60);          // 与 watcher 的合并窗口叠加后仍 <= ~150 ms
        connect(m_eventTimer, &QTimer::timeout, this, [this] {
            QVector<FileSystemEvent> batch;
            batch.swap(m_pendingEvents);
            applyEventBatch(batch);
        });
    }

    m_subscription = m_dispatcher->subscribe([this](const FileSystemEvent &ev) {
        // 约定：dispatcher 在 watcher 所在线程（GUI）发布事件
        m_pendingEvents.push_back(ev);
        if (m_eventTimer && !m_eventTimer->isActive())
            m_eventTimer->start();
    });

    Logger::get().info("FileIndex: 已接入增量事件源（subscription {}）", m_subscription);
}

void FileIndex::applyEventBatch(const QVector<FileSystemEvent> &batch)
{
    if (batch.isEmpty())
        return;

    for (const FileSystemEvent &ev : batch) {
        if (ev.type == FileSystemEvent::Overflow) {
            Logger::get().warn("FileIndex: 收到 Overflow（{}），转为全量重扫",
                               ev.path.toStdString());
            startAsync();
            return;
        }
    }

    // 扫描期间的事件不单独处理：这轮扫描结束时会发布磁盘现状
    if (m_scanning) {
        m_pendingRefresh = true;
        return;
    }

    // 大批量（构建产物、解压）不值得逐条打补丁，直接重扫更稳
    if (batch.size() > 2000) {
        Logger::get().info("FileIndex: 事件批过大（{}），转为全量重扫", batch.size());
        startAsync();
        return;
    }

    if (m_patchBusy.exchange(true)) {
        m_pendingRefresh = true;      // 上一批还没算完，交给扫描兜底
        return;
    }

    QThread *thread = QThread::create([this, batch] { patchSnapshotInWorker(batch); });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void FileIndex::patchSnapshotInWorker(QVector<FileSystemEvent> batch)
{
    const std::shared_ptr<const FileSnapshot> current = m_snapshots->current();
    if (!current) {
        m_patchBusy.store(false);
        return;
    }

    // 同一路径在一批里可能多次出现：后来的覆盖先前的；Renamed 拆成 删旧 + 加新
    QHash<QString, FileSystemEvent> changes;
    for (const FileSystemEvent &ev : batch) {
        if (ev.type == FileSystemEvent::Renamed) {
            FileSystemEvent removed;
            removed.type = FileSystemEvent::Removed;
            removed.path = ev.oldPath;
            changes.insert(ev.oldPath.toLower(), removed);

            FileSystemEvent added;
            added.type = FileSystemEvent::Added;
            added.path = ev.path;
            changes.insert(ev.path.toLower(), added);
        } else {
            changes.insert(ev.path.toLower(), ev);
        }
    }

    FileSnapshot::Builder builder;
    builder.reserve(current->size() + changes.size());

    QSet<QString> handled;
    handled.reserve(changes.size());

    auto rowFor = [](const QString &path) {
        FileIndexDatabase::FileRow row;
        const QFileInfo info(path);
        row.path  = path;
        row.name  = info.fileName();
        row.size  = info.size();
        row.mtime = info.lastModified().toSecsSinceEpoch();
        return row;
    };

    FileIndexDatabase::Diff diff;

    for (int i = 0; i < current->size(); ++i) {
        const QString path = current->pathAt(i);
        const QString key  = path.toLower();
        auto it = changes.constFind(key);
        if (it == changes.constEnd()) {
            builder.add(current->nameAt(i), path, current->sizeAt(i), current->mtimeAt(i));
            continue;
        }

        handled.insert(key);
        if (it->type == FileSystemEvent::Removed)
            continue;                                  // 从快照里消失

        const QFileInfo info(path);
        if (!info.isFile())
            continue;                                  // 目录/已消失：不进索引
        builder.add(info.fileName(), path, info.size(), info.lastModified().toSecsSinceEpoch());
        if (it->type == FileSystemEvent::Modified)
            diff.changed.append(rowFor(path));
    }

    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        if (handled.contains(it.key()))
            continue;
        if (it->type == FileSystemEvent::Removed) {
            diff.removed.append(it->path);
            continue;
        }
        const QFileInfo info(it->path);
        if (!info.isFile())
            continue;
        builder.add(info.fileName(), it->path, info.size(),
                    info.lastModified().toSecsSinceEpoch());
        diff.added.append(rowFor(it->path));
    }

    std::shared_ptr<const FileSnapshot> patched = builder.finish();

    if (!m_databasePath.isEmpty() && !diff.empty()) {
        FileIndexDatabase db(QStringLiteral("file_index_patch"));
        if (db.open(m_databasePath)) {
            if (!db.applyDiff(diff))
                Logger::get().warn("FileIndex: 增量写库失败 — {}",
                                   db.lastError().toStdString());
            db.close();
        }
    }

    QMetaObject::invokeMethod(this, [this, patched, batch] {
        publishPatchedSnapshot(patched, batch.size());
    }, Qt::QueuedConnection);
}

void FileIndex::publishPatchedSnapshot(std::shared_ptr<const FileSnapshot> snapshot, int changedEvents)
{
    m_patchBusy.store(false);

    if (!snapshot)
        return;

    m_snapshots->publish(std::move(snapshot));
    m_msLastScan = 0;

    Logger::get().info("FileIndex: 增量补丁 — {} 个事件，快照 #{} 现有 {} 条",
                       changedEvents, m_snapshots->generation(), m_snapshots->current()->size());
    emit indexStateChanged();
}
