#include "core/FileIndex.h"

#include "core/FileSnapshot.h"
#include "core/SnapshotManager.h"
#include "core/FileScanner.h"
#include "core/QueryEngine.h"
#include "core/Logger.h"
#include "database/FileIndexDatabase.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>

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
    ScanWorker(int entryLimit, QString databasePath, QObject *parent = nullptr)
        : QThread(parent)
        , m_entryLimit(entryLimit > 0 ? entryLimit : kHardLimitEntries)
        , m_databasePath(std::move(databasePath))
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

        QVector<FileScanner::Entry> entries;
        scan = FileScanner::scan(FileScanner::defaultRoots(), options, &entries,
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
                for (const QString &root : FileScanner::defaultRoots()) {
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
    startScan();
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

    m_worker = std::make_unique<ScanWorker>(m_entryLimit, m_databasePath);
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
    return s;
}

bool FileIndex::isReady() const
{
    return static_cast<bool>(m_snapshots->current());
}
