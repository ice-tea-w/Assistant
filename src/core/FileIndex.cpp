#include "core/FileIndex.h"

#include "core/FileSnapshot.h"
#include "core/SnapshotManager.h"
#include "core/FuzzyMatcher.h"
#include "core/Logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <vector>

namespace {

/// §3.3 的两档阈值（**绝不静默驱逐**）：
/// - 软预算：超了**继续索引**，只在日志里说明实际规模与"怎么缩小"；
/// - 硬保护：超了**停止索引**（不驱逐已有条目）并醒目告警。
/// 控制规模的手段是"少收"（roots / 排除规则 / 种类白名单），不是"事后丢"。
constexpr int kSoftBudgetEntries = 300000;
constexpr int kHardLimitEntries  = 1000000;

/// 目录级剪枝名单，与 `FileSearcher` 保持一致（A1 的 P0-2）。
/// ⚠️ A2 期间这里与 `src/core/FileSearcher.cpp` 的 `kSkippedDirs` 是**两份**，
/// A3 把扫描换成 `FileScanner` 后只留一份。
const QStringList kSkippedDirs = {
    QStringLiteral("node_modules"),
    QStringLiteral("__pycache__"),
    QStringLiteral(".cache"),
    QStringLiteral(".git"),
    QStringLiteral(".svn"),
};

} // namespace

/// A2 的扫描器：**临时实现**，语义与 A1 的 `FileSearcher::walkDir` 一致
/// （前序 DFS、不跟随 reparse point、目录级剪枝、跳过隐藏项），但做了两件
/// 旧实现不能做的事：
///   1. 在 **worker 线程**上跑，构建一份全新快照（GUI 线程不阻塞）；
///   2. 采集 `size` / `mtime`（A3 的差量与治理反查都要），且**不做 2 万条
///      任意截断** —— 那是 P0-3：本机 Desktop 就有 3.5 万文件，旧上限让
///      Documents/Downloads 一个文件都进不来。
///
/// A3 会用 Win32 `FindFirstFileExW`（`FindExInfoBasic` + `LARGE_FETCH`）的
/// 迭代式 DFS 换掉整个 `run()`：一次系统调用批量拿到 name/size/mtime，
/// 且路径走栈式缓冲、零逐条分配。
class ScanWorker : public QThread
{
public:
    /// @param entryLimit 硬保护上限；<= 0 用 §3.3 的默认值
    explicit ScanWorker(int entryLimit, QObject *parent = nullptr)
        : QThread(parent)
        , m_entryLimit(entryLimit > 0 ? entryLimit : kHardLimitEntries)
    {
    }

    /// 请求取消（析构/退出时用，避免"退出要等扫描跑完"）。
    void cancel() { m_cancel.store(true, std::memory_order_relaxed); }

    // ---- run() 写，GUI 线程在 finished 之后读 ----
    std::shared_ptr<FileSnapshot> snapshot;
    qint64 elapsedMs = 0;

protected:
    void run() override
    {
        QElapsedTimer timer;
        timer.start();

        // 常用用户目录 —— 与旧 FileSearcher 的 roots 一致；家目录本身刻意排除
        // （里面有 config/AppData 噪音）。
        const QStringList roots = {
            QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
            QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
        };

        QSet<QString> seenRoots;   // 重叠 roots（junction/重定向）的廉价保险
        for (const QString &root : roots) {
            if (cancelled())
                break;
            if (root.isEmpty() || seenRoots.contains(root))
                continue;
            seenRoots.insert(root);
            if (!QDir(root).exists())
                continue;

            walk(root);
            if (m_hitHardLimit)
                break;
        }

        const int scannedFiles = m_builder.count();
        snapshot  = m_builder.finish();
        elapsedMs = timer.elapsed();

        if (m_prunedDirs > 0)
            Logger::get().debug("FileIndex: pruned {} excluded dir(s) (node_modules etc.)",
                                m_prunedDirs);
        if (m_hitHardLimit)
            Logger::get().warn("FileIndex: 已达硬保护上限 {} 条，停止索引（**不驱逐**已有条目）；"
                               "建议在设置里收窄 roots 或补排除规则", m_entryLimit);
        else if (scannedFiles >= kSoftBudgetEntries)
            Logger::get().warn("FileIndex: 条目数 {} 已超软预算 {}（继续索引，仅提示）",
                               scannedFiles, kSoftBudgetEntries);
    }

private:
    bool cancelled() const { return m_cancel.load(std::memory_order_relaxed); }

    /// 前序 DFS，与 A1 的 `walkDir` 同序（顺序变了会改变 top-N 截断结果）。
    void walk(const QString &dirPath)
    {
        const QFileInfoList entries =
            QDir(dirPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                        QDir::NoSort);   // NoSort = 保持文件系统原始顺序
        for (const QFileInfo &info : entries) {
            if (cancelled())
                return;
            if (m_builder.count() >= m_entryLimit) {
                m_hitHardLimit = true;
                return;
            }

            if (info.isDir()) {
                // P0-1：不跟随 reparse point（Qt 的 isSymLink() 不认 junction，
                // 6.2 起才有 isJunction()）
                if (info.isSymLink() || info.isJunction())
                    continue;

                // P0-2：命中即整棵子树跳过 —— 剪枝发生在**下降之前**
                if (kSkippedDirs.contains(info.fileName(), Qt::CaseInsensitive)) {
                    ++m_prunedDirs;
                    continue;
                }

                walk(info.filePath());
            } else {
                if (info.isSymLink())
                    continue;

                // QDir::Hidden 不设 → 隐藏文件/隐藏目录本就枚举不到（沿用既有语义）
                m_builder.add(info.fileName(), info.filePath(),
                              info.size(), info.lastModified().toSecsSinceEpoch());
            }
        }
    }

    std::atomic<bool> m_cancel{false};
    FileSnapshot::Builder m_builder;
    const int m_entryLimit;
    bool m_hitHardLimit = false;
    int  m_prunedDirs   = 0;
};

// ---------------------------------------------------------------------------
// FileIndex
// ---------------------------------------------------------------------------

FileIndex::FileIndex(QObject *parent)
    : QObject(parent)
    , m_snapshots(std::make_unique<SnapshotManager>())
{
}

FileIndex::~FileIndex()
{
    // 不能带着还在跑的线程析构：先请它收手，再等。扫描在目录粒度检查取消标志，
    // 所以这里最多等"当前目录枚举完"的那点时间。
    if (m_worker) {
        m_worker->cancel();
        m_worker->wait();
    }
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
        // 所以现在回收是安全的；不能在上面的槽里删，那是它自己在发信号。
        m_worker->wait();
        m_worker.reset();
    }

    m_worker = std::make_unique<ScanWorker>(m_entryLimit);
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
        const int entries = snap ? snap->size() : 0;

        m_snapshots->publish(std::move(snap));

        Logger::get().info("FileIndex: published snapshot #{} — {} files in {} ms",
                           m_snapshots->generation(), entries, m_msLastScan);

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

    const std::shared_ptr<const FileSnapshot> snap = m_snapshots->current();
    if (!snap || text.trimmed().isEmpty() || limit <= 0)
        return out;

    // A2 的查询 = 旧 FileSearcher::search() 的算法原样搬过来（线性扫描 +
    // FuzzyMatcher + 稳定序），**只换成走快照的只读接口**。A4 才把它拆成
    // QueryEngine（Parser → CandidateFilter → Score → TopK 堆），届时这里
    // 瘦成一行转发。
    struct Hit { int index; int score; };

    std::vector<Hit> hits;
    hits.reserve(1024);
    for (int i = 0, n = snap->size(); i < n; ++i) {
        // lowerAt() 返回预折叠小写的 const 引用（零拷贝）；FuzzyMatcher 内部
        // 还会再 toLower 一次，结果与传原始大小写完全一致。
        const int s = FuzzyMatcher::score(text, snap->lowerAt(i));
        if (s > 0)
            hits.push_back({i, s});
    }

    // 分数降序；同分按名字（大小写不敏感序 ≈ 比较已折叠小写的名字）。
    // 与旧实现的 tie-break 等价，但零分配。
    std::sort(hits.begin(), hits.end(), [&snap](const Hit &a, const Hit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return snap->lowerAt(a.index) < snap->lowerAt(b.index);
    });

    const int take = std::min<int>(limit, static_cast<int>(hits.size()));
    out.reserve(take);
    for (int k = 0; k < take; ++k) {
        const QString path = snap->pathAt(hits[k].index);

        SearchResult item;
        item.id       = QStringLiteral("file://") + path;
        item.title    = snap->nameAt(hits[k].index);
        item.subtitle = path;
        item.type     = SearchResult::File;
        item.priority = 1;              // 与内建/应用同分时排在后面
        item.score    = hits[k].score;
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
