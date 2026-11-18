#include "core/FileIndex.h"

#include "core/FileEventDispatcher.h"
#include "core/FileClassifier.h"
#include "core/FileRules.h"
#include "core/FileSnapshot.h"
#include "core/SnapshotManager.h"
#include "core/FileScanner.h"
#include "core/QueryEngine.h"
#include "core/Logger.h"
#include "database/FileIndexDatabase.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <functional>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace {

/// §3.3 的两档阈值（**绝不静默驱逐**）：
/// - 软预算：超了**继续索引**，只在日志里说明实际规模与"怎么缩小"；
/// - 硬保护：超了**停止索引**（不驱逐已有条目）并醒目告警。
/// 控制规模的手段是"少收"（roots / 排除规则 / 种类白名单），不是"事后丢"。
constexpr int kSoftBudgetEntries = 300000;
constexpr int kHardLimitEntries  = 1000000;

/// 增量校正的最短间隔：不足 24h 的启动**一条都不枚举**（缓存 + watcher 足够）。
/// 想立刻重扫走设置页「重建索引」（`FileIndex::rebuild()`，忽略这个阈值）。
constexpr qint64 kIncrementalIntervalSec = 24 * 3600;

/// 判定"用户不活跃"的空闲秒数：到点后的自动校正只在这个窗口里启动。
constexpr int kIdleSecondsRequired = 45;

/// 空闲闸门的重看间隔与等待上限：15 分钟内一直不空闲也照跑，
/// 否则"天天挂着电脑"的机器永远等不到校正。
constexpr int kScanRetryMs   = 30 * 1000;
constexpr int kMaxScanDeferMs = 15 * 60 * 1000;

QString toLongPathWin(const QString &normal)
{
    QString win = normal;
    win.replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (win.startsWith(QStringLiteral("\\\\?\\")))
        return win;
    if (win.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + win.mid(2);
    return QStringLiteral("\\\\?\\") + win;
}

/// 目录 mtime，单位是 **FILETIME tick（100ns）**，不是 Unix 秒。
///
/// 为什么不用秒：目录 mtime 只用于"和上次比是否相等"。截到秒会让**同一秒内**
/// 的增删判成"没变"（扫描刚结束、用户马上存一个文件就会漏掉，测试里也复现了）。
/// tick 是 100ns 粒度，直接比原始 64 位值即可；这个数只在本模块内比较，不展示。
///
/// 失败（目录没了/打不开）时 *ok = false，调用方一律当"脏"处理 ——
/// 宁可多扫一棵子树，也不要漏掉新增文件。
qint64 dirMtimeTicks(const QString &normal, bool *ok)
{
    if (ok)
        *ok = false;

    WIN32_FILE_ATTRIBUTE_DATA data{};
    const QString longPath = toLongPathWin(normal);
    if (!GetFileAttributesExW(reinterpret_cast<LPCWSTR>(longPath.utf16()),
                              GetFileExInfoStandard, &data))
        return 0;
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;

    ULARGE_INTEGER ticks;
    ticks.LowPart  = data.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
    if (ok)
        *ok = true;
    return static_cast<qint64>(ticks.QuadPart);
}

QString parentDirOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QString();
    return path.left(slash);
}

/// path 是否等于 dir 或位于 dir 之下（大小写不敏感；都存 `/` 分隔的普通形式）。
bool isUnderPath(const QString &path, const QString &dir)
{
    if (dir.isEmpty() || path.size() < dir.size())
        return false;
    if (path.compare(dir, Qt::CaseInsensitive) != 0 &&
        !path.startsWith(dir, Qt::CaseInsensitive))
        return false;
    if (path.size() == dir.size())
        return true;
    return path.at(dir.size()) == QLatin1Char('/');
}

bool isUnderAnyRoot(const QString &path, const QStringList &roots)
{
    for (const QString &root : roots) {
        if (isUnderPath(path, root))
            return true;
    }
    return false;
}

/// 扫描线程降级：CPU 最低 + 后台 I/O 模式（别和 UI 抢磁盘队列）。
/// 失败不致命 —— 只是回到默认优先级。
void applyBackgroundPriority()
{
    HANDLE thread = GetCurrentThread();
    SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);
    SetThreadPriority(thread, THREAD_MODE_BACKGROUND_BEGIN);
}

/// 用户是不是已经空闲够久（会话级 GetLastInputInfo，包住所有应用）。
bool userIsIdle()
{
    LASTINPUTINFO info{};
    info.cbSize = sizeof(LASTINPUTINFO);
    if (!GetLastInputInfo(&info))
        return true;                       // 拿不到就当空闲，别把校正饿死
    const DWORD idleMs = GetTickCount() - info.dwTime;   // 无符号回绕安全
    return idleMs >= static_cast<DWORD>(kIdleSecondsRequired) * 1000u;
}

/// 增量计划：要重新枚举的子树 + verify walk 期间拿到的目录 mtime。
struct IncrementalPlan
{
    QStringList            dirtyRoots;
    QHash<QString, qint64> mtimes;
};

/// 对每棵已知目录做一次 stat，mtime 变了的目录成为"脏子树"（不再下探，
/// 交给 `FileScanner` 整棵重扫）；没变的继续下探它的子目录。
///
/// 为什么这能省时间：目录 mtime 只在**直接子项增删/改名**时变。因此没变的目录
/// 不需要 `FindFirstFile` 枚举它 —— 一次 `GetFileAttributesEx` 就够。
/// 已知漏点：关机期间"原地修改"的文件不改变父目录 mtime，快照里的 size/mtime
/// 会偏旧（搜索结果仍在），由 24h 的全量校正与 watcher 兜底。
IncrementalPlan buildIncrementalPlan(const QVector<FileIndexDatabase::DirRow> &dirs,
                                     const QStringList &roots)
{
    QHash<QString, qint64> known;
    known.reserve(dirs.size());
    QHash<QString, QStringList> children;
    children.reserve(dirs.size());

    for (const FileIndexDatabase::DirRow &dir : dirs) {
        known.insert(dir.path, dir.mtime);
        children[parentDirOf(dir.path)].append(dir.path);
    }

    IncrementalPlan plan;
    QSet<QString> visited;

    // 迭代而不是递归：目录树深到几十层也不会把栈打穿
    QStringList stack;
    for (const QString &root : roots) {
        if (!root.isEmpty())
            stack.append(root);
    }

    while (!stack.isEmpty()) {
        const QString dir = stack.takeLast();
        if (visited.contains(dir))
            continue;
        visited.insert(dir);

        bool ok = false;
        const qint64 mtime = dirMtimeTicks(dir, &ok);
        if (!ok) {
            // 目录没了 / 打不开：整棵重扫（scanner 会跳过不存在的），
            // 它下面的旧行因为"不属于干净目录"会被 diff 判删除。
            plan.dirtyRoots.append(dir);
            continue;
        }
        plan.mtimes.insert(dir, mtime);

        const auto it = known.constFind(dir);
        if (it == known.constEnd() || it.value() == 0 || it.value() != mtime) {
            plan.dirtyRoots.append(dir);      // 新目录 / 从没 stamp 过 / 真的变了
            continue;
        }

        const auto childIt = children.constFind(dir);
        if (childIt != children.constEnd()) {
            for (const QString &child : childIt.value())
                stack.append(child);
        }
    }

    // 父子同时脏时只留父：多扫一棵子树无所谓，重复扫同一批文件不行
    if (plan.dirtyRoots.size() > 1) {
        std::sort(plan.dirtyRoots.begin(), plan.dirtyRoots.end(),
                  [](const QString &a, const QString &b) { return a.size() < b.size(); });
        QStringList pruned;
        pruned.reserve(plan.dirtyRoots.size());
        for (const QString &candidate : plan.dirtyRoots) {
            if (!isUnderAnyRoot(candidate, pruned))
                pruned.append(candidate);
        }
        plan.dirtyRoots = pruned;
    }

    return plan;
}

} // namespace

/// 扫描工作线程：采集（`FileScanner`）+ 差量写库（`FileIndexDatabase`）+ 建快照。
///
/// 三件事都放在这里，是因为 `QSqlDatabase` 的连接**线程亲和**：连接在 `run()` 里
/// 建立、在同一个 `run()` 里销毁，所以差量写库不会卡住 GUI 线程（§3.4）。
/// 门面只在 `finished` 之后收结果 —— 那时 `run()` 已返回，两者之间的先后有
/// Qt 的排队投递兜底。
///
/// 2026-11-19 起多了"增量校正"一条路径：
///  - `Mode::Auto`：距上次扫描不足 24h → 一条都不枚举；到点后只重扫
///    "目录 mtime 变了"的子树（`dirs.mtime` 是判据）。
///  - `Mode::Rebuild`：忽略 24h 与 mtime，全量扫（设置页「重建索引」）。
class ScanWorker : public QThread
{
public:
    enum class Mode { Auto, Rebuild };

    ScanWorker(int entryLimit, QString databasePath, QStringList roots,
               FileClassifier classifier, ExcludeRuleSet rules, Mode mode,
               QObject *parent = nullptr)
        : QThread(parent)
        , m_entryLimit(entryLimit > 0 ? entryLimit : kHardLimitEntries)
        , m_databasePath(std::move(databasePath))
        , m_roots(std::move(roots))
        , m_classifier(std::move(classifier))
        , m_rules(std::move(rules))
        , m_mode(mode)
    {
    }

    /// 请求取消（析构/退出时用，避免"退出要等扫描跑完"）。
    void cancel() { m_cancel.store(true, std::memory_order_relaxed); }

    // ---- run() 写，GUI 线程在 finished 之后读 ----
    std::shared_ptr<FileSnapshot> snapshot;   ///< 空 = 这次没有新快照（跳过 / 全部复用）
    qint64                    elapsedMs = 0;
    FileScanner::Stats        scan;
    FileIndexDatabase::Diff   diff;
    bool                      dbOpened = false;
    int                       dbRows   = 0;
    QString                   dbError;
    bool                      skipped   = false;  ///< 24h 策略：这次没扫
    bool                      reusedAll = false;  ///< 增量：目录一条没变，零枚举
    int                       dirtyDirs = 0;      ///< 增量：重扫了几棵子树

protected:
    void run() override
    {
        QElapsedTimer timer;
        timer.start();

        applyBackgroundPriority();

        const QStringList roots = m_roots.isEmpty() ? FileScanner::defaultRoots() : m_roots;

        FileScanner::Options options;
        options.maxEntries = m_entryLimit;
        options.rules      = &m_rules;
        options.classifier = &m_classifier;

        // 验收工具：不持久化，只能全扫
        if (m_databasePath.isEmpty()) {
            scanAll(roots, options);
            elapsedMs = timer.elapsed();
            return;
        }

        FileIndexDatabase db;
        if (!db.open(m_databasePath)) {
            dbError = db.lastError();
            Logger::get().warn("FileIndex: 打开库失败，本次按无库全扫 — {}", dbError.toStdString());
            scanAll(roots, options);
            elapsedMs = timer.elapsed();
            return;
        }
        dbOpened = true;

        const qint64 lastScan = db.lastScanAt();
        const qint64 now      = QDateTime::currentSecsSinceEpoch();

        // 24h 策略：不足一天 → 一条都不枚举。**先查闸门再读全表**：
        // 跳过时只花一条 SELECT MAX(last_scan) + 一次 COUNT(*)，不碰 6700 行文件表。
        if (m_mode == Mode::Auto && lastScan > 0
            && now - lastScan < kIncrementalIntervalSec) {
            const int rows = db.rowCount();
            if (rows > 0) {
                skipped = true;
                dbRows  = rows;
                db.close();
                elapsedMs = timer.elapsed();
                return;
            }
        }

        const QVector<FileIndexDatabase::FileRow> dbRowsVec = db.loadAll();
        const QVector<FileIndexDatabase::DirRow>  dirs      = db.loadDirs();

        // 空库 / 目录一次都没 stamp 过 → 全扫，扫完就建立了增量基线
        if (dbRowsVec.isEmpty() || dirs.isEmpty()) {
            scanAllWithDb(db, roots, options, now);
            dbRows = db.rowCount();
            db.close();
            elapsedMs = timer.elapsed();
            return;
        }

        // ---- 24h 到了：先只看目录 mtime，决定"这次到底要不要扫" ----
        //
        // 之所以不"只重扫变了的子树"：脏目录被当成扫描根时会丢掉父级上下文
        // （目录级排除名、工程哨兵、demote 继承），实测把 Desktop/vcpkg 这类
        // 本该剪掉的树当根扫进来，20k 行凭空多出 13k。所以 mtime 只做**闸门**：
        //   - 一条没变 → 不扫（只 stamp + 推 last_scan）；
        //   - 有变化   → 走完整全扫（语义与首扫完全一致，不引入第二套判定）。
        // 代价是"一天最多一次全扫"，而它已经在空闲窗口里跑、且在后台线程。
        IncrementalPlan plan = buildIncrementalPlan(dirs, roots);
        dirtyDirs = plan.dirtyRoots.size();

        if (plan.dirtyRoots.isEmpty()) {
            reusedAll = true;
            if (!db.stampDirMtimes(plan.mtimes))
                dbError = db.lastError();
            for (const QString &root : roots) {
                if (!root.isEmpty())
                    db.touchRoot(root, now);
            }
            dbRows = db.rowCount();
            db.close();
            elapsedMs = timer.elapsed();
            return;                       // snapshot 空 → 门面保留缓存快照
        }

        Logger::get().info("FileIndex: 目录变动 {} 处，转为全量校正", dirtyDirs);
        scanAllWithDb(db, roots, options, now);
        dbRows = db.rowCount();
        db.close();
        elapsedMs = timer.elapsed();
    }

private:
    void scanAll(const QStringList &roots, const FileScanner::Options &options)
    {
        QVector<FileScanner::Entry> entries;
        scan = FileScanner::scan(roots, options, &entries, [this] {
            return m_cancel.load(std::memory_order_relaxed);
        });
        logScanStats(QString());
        snapshot = buildSnapshotFromEntries(entries);
    }

    void scanAllWithDb(FileIndexDatabase &db, const QStringList &roots,
                       const FileScanner::Options &options, qint64 now)
    {
        // visitedDirs：把这次真正打开过的**每一个**目录（含空目录）记下来，
        // 扫完统一 stamp mtime。只 stamp "有文件的目录"会让空目录永远停在
        // mtime=0，下次必然判脏 —— 增量闸门就白设了。
        QVector<QString> visited;
        FileScanner::Options opts = options;
        opts.visitedDirs = &visited;

        QVector<FileScanner::Entry> entries;
        scan = FileScanner::scan(roots, opts, &entries, [this] {
            return m_cancel.load(std::memory_order_relaxed);
        });
        logScanStats(QString());

        QVector<FileIndexDatabase::FileRow> diskRows;
        diskRows.reserve(entries.size());
        for (const FileScanner::Entry &e : entries) {
            FileIndexDatabase::FileRow row;
            row.path    = e.path();
            row.name    = e.name;
            row.size    = e.size;
            row.mtime   = e.mtime;
            row.catMask = e.catMask;
            row.flags   = e.flags;
            row.rootId  = e.rootId;
            diskRows.append(std::move(row));
        }

        diff = FileIndexDatabase::diff(diskRows, db.loadAll());
        if (!diff.empty() && !db.applyDiff(diff))
            dbError = db.lastError();

        // 全扫是增量基线的起点：扫到过的目录（含空目录）全部 stamp 当前 mtime
        QHash<QString, qint64> stamps;
        stamps.reserve(visited.size() + 16);
        for (const QString &dir : visited) {
            if (dir.isEmpty() || stamps.contains(dir))
                continue;
            bool ok = false;
            const qint64 mtime = dirMtimeTicks(dir, &ok);
            if (ok)
                stamps.insert(dir, mtime);
        }
        if (!db.stampDirMtimes(stamps))
            dbError = db.lastError();

        for (const QString &root : roots) {
            if (!root.isEmpty())
                db.touchRoot(root, now);
        }

        snapshot = buildSnapshotFromEntries(entries);
    }

    void logScanStats(const QString &tag)
    {
        const std::string suffix = tag.isEmpty() ? std::string()
                                                 : std::string("(") + tag.toStdString() + ")";
        Logger::get().info("FileScanner{}: {} files / {} dirs in {} ms "
                           "(pruned {}, projectSkip {}, projectDemote {}, fileRules {}, "
                           "denied {}, cancelled={}, hitLimit={})",
                           suffix,
                           scan.files, scan.dirsVisited, scan.elapsedMs,
                           scan.prunedDirs, scan.projectDirsSkipped, scan.projectDirsDemoted,
                           scan.filesSkipped, scan.deniedDirs,
                           scan.cancelled, scan.hitHardLimit);
    }

    std::shared_ptr<FileSnapshot> buildSnapshotFromEntries(const QVector<FileScanner::Entry> &entries)
    {
        FileSnapshot::Builder builder;
        builder.reserve(entries.size());
        for (const FileScanner::Entry &e : entries)
            builder.add(e.name, e.path(), e.size, e.mtime, e.catMask, e.flags, e.rootId);
        return builder.finish();
    }

    std::atomic<bool> m_cancel{false};
    const int         m_entryLimit;
    const QString     m_databasePath;
    const QStringList m_roots;   ///< 空 = FileScanner::defaultRoots()
    FileClassifier    m_classifier;
    ExcludeRuleSet    m_rules;
    const Mode        m_mode;
};

// ---------------------------------------------------------------------------
// FileIndex
// ---------------------------------------------------------------------------

FileIndex::FileIndex(QObject *parent)
    : QObject(parent)
    , m_snapshots(std::make_unique<SnapshotManager>())
    , m_query(std::make_unique<QueryEngine>(*m_snapshots))
    , m_classifier(std::make_unique<FileClassifier>())
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
    // 界面立刻可查；随后再**按策略**校正（见 scheduleAutoScan 的注释）。
    if (!m_snapshots->current() && !m_databasePath.isEmpty()
        && QFileInfo::exists(m_databasePath)) {
        startColdLoad();
        return;
    }

    // 连缓存都没有（首启/删库）：直接扫。让用户空等 30s 空闲才出结果，比扫一下更糟。
    if (!m_snapshots->current()) {
        startScan(false);
        return;
    }

    scheduleAutoScan();
}

void FileIndex::scheduleAutoScan()
{
    // 自动校正的三个闸门（2026-11-19）：
    //  1. 24h —— 不足一天连 worker 都不起（起 worker 也只读几行就跳过，纯属浪费）；
    //  2. 不活跃 —— 用户正在打字/点界面时不抢磁盘，每 30s 再看一次；
    //  3. 兜底上限 —— 等了 15 分钟仍不空闲就跑（否则一直活跃的机器永远不校正）。
    // 手动「重建索引」走 rebuild()，两个闸门都不看。
    if (m_scanTimer) {
        m_scanTimer->stop();
    }

    if (m_pendingScanForce) {
        tryStartScheduledScan();
        return;
    }

    // 冷启动时已经读过 roots.last_scan：不足 24h 就在**这里**结案，
    // 连 worker 都不必起（起 worker 只为读一行再退出，白花一次线程 + 一次开库）。
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (m_lastScanAt > 0 && now - m_lastScanAt < kIncrementalIntervalSec) {
        Logger::get().info("FileIndex: 跳过校正（距上次扫描 {} 分钟，阈值 24 小时）",
                           (now - m_lastScanAt) / 60);
        emit scanSkipped(QStringLiteral("距上次扫描不足 24 小时"));
        emit indexStateChanged();
        emit indexReady(m_snapshots->current() ? m_snapshots->current()->size() : 0,
                        m_msLastScan);
        // 常驻进程也要在满 24h 时自己醒一次（不是只有启动才校正）
        const qint64 remainMs = (m_lastScanAt + kIncrementalIntervalSec - now) * 1000;
        const qint64 clamped = remainMs < 60 * 1000 ? 60 * 1000
                              : (remainMs > 12 * 3600 * 1000 ? 12 * 3600 * 1000 : remainMs);
        ensureScanTimer()->start(static_cast<int>(clamped));
        return;
    }

    const bool idle = userIsIdle();
    Logger::get().debug("FileIndex: 校正调度 idle={} 已等={}ms defer={}",
                        idle, m_scanWaitMs, m_scanDefer);

    if (m_scanDefer && !idle && m_scanWaitMs < kMaxScanDeferMs) {
        m_scanWaitMs += kScanRetryMs;
        ensureScanTimer()->start(kScanRetryMs);
        return;
    }

    tryStartScheduledScan();
}

QTimer *FileIndex::ensureScanTimer()
{
    if (!m_scanTimer) {
        m_scanTimer = new QTimer(this);
        m_scanTimer->setSingleShot(true);
        connect(m_scanTimer, &QTimer::timeout, this, &FileIndex::scheduleAutoScan);
    }
    return m_scanTimer;
}

void FileIndex::tryStartScheduledScan()
{
    m_scanWaitMs = 0;
    if (m_scanTimer)
        m_scanTimer->stop();

    const bool forceFull = m_pendingScanForce;
    m_pendingScanForce = false;
    startScan(forceFull);
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
    qint64 lastScanAt = 0;
    {
        FileIndexDatabase db(QStringLiteral("file_index_cold"));
        if (db.open(m_databasePath)) {
            lastScanAt = db.lastScanAt();
            const auto all = db.loadAll();
            rows = static_cast<int>(all.size());
            builder.reserve(rows);
            for (const auto &row : all) {
                // 归类始终以当前分类器为准：v1→v2 旧行是 0，用户改 override 也不用等重扫。
                // 白名单不在这里筛 —— 检索期由 QueryEngine 判，切换白名单不重建快照。
                const uint16_t mask = m_classifier->classify(row.name);
                builder.add(row.name, row.path, row.size, row.mtime,
                            mask, row.flags, row.rootId);
            }
            db.close();
        }
    }

    auto snapshot = builder.finish();
    m_dbRows    = rows;
    m_coldMs    = timer.elapsed();
    m_fromCache = true;
    m_scanning  = false;
    m_lastScanAt = lastScanAt;

    m_snapshots->publish(snapshot);
    const int entries = snapshot->size();

    Logger::get().info("FileIndex: 冷启动从 DB 载入 {} 条（{} ms），立即可查；随后按策略校正",
                       entries, m_coldMs);
    emit cacheLoaded(entries, m_coldMs);
    emit indexStateChanged();

    scheduleAutoScan();            // 校正（24h + 空闲闸门）
}

void FileIndex::setScanDeferEnabled(bool enabled)
{
    m_scanDefer = enabled;
}

void FileIndex::refresh()
{
    // 采集规则/根目录变了：必须重扫，且不能等 24h —— 走强制全量。
    rebuild();
}

void FileIndex::rebuild()
{
    m_pendingScanForce = true;

    if (m_scanning) {
        m_pendingRefresh = true;
        return;
    }
    // 手动重建不看空闲：用户刚点的按钮，就该立刻动
    tryStartScheduledScan();
}

void FileIndex::startScan(bool forceFull)
{
    if (m_worker) {
        // 上一轮的 finished 信号早已发完（m_scanning 为假才会走到这里），
        // 所以现在回收是安全的；不能在下面的槽里删，那是它自己在发信号。
        m_worker->wait();
        m_worker.reset();
    }

    m_worker = std::make_unique<ScanWorker>(
        m_entryLimit, m_databasePath, m_roots, *m_classifier, m_rules,
        forceFull ? ScanWorker::Mode::Rebuild : ScanWorker::Mode::Auto);
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

        const bool noNewSnapshot = (snap == nullptr);
        const int entries = noNewSnapshot
                                ? (m_snapshots->current() ? m_snapshots->current()->size() : 0)
                                : snap->size();

        if (noNewSnapshot) {
            // 24h 内跳过 / 目录全都没变：**保留冷启动快照**，不发布空快照
            const QString why = m_worker->skipped
                                    ? QStringLiteral("距上次扫描不足 24 小时")
                                    : QStringLiteral("目录 mtime 全部未变");
            Logger::get().info("FileIndex: 跳过校正（{}），继续用 {} 条缓存（{} ms，脏子树 {}）",
                               why.toStdString(), entries, m_msLastScan, m_worker->dirtyDirs);
            emit scanSkipped(why);
        } else {
            m_fromCache = false;
            m_lastScanAt = QDateTime::currentSecsSinceEpoch();   // 24h 闸门的起点
            m_snapshots->publish(std::move(snap));

            Logger::get().info("FileIndex: published snapshot #{} — {} files in {} ms (db {} rows)",
                               m_snapshots->generation(), entries, m_msLastScan, m_dbRows);
        }

        emit indexStateChanged();
        emit indexReady(entries, m_msLastScan);
    }

    if (m_pendingRefresh) {
        m_pendingRefresh = false;
        if (m_pendingScanForce)
            tryStartScheduledScan();
        else
            scheduleAutoScan();
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

// ---------------------------------------------------------------------------
// Phase B：治理注入
// ---------------------------------------------------------------------------

void FileIndex::setClassifier(const FileClassifier &classifier)
{
    *m_classifier = classifier;

    // 分类器只用在下一次“建快照/归类”时。典型调用点是 main.cpp 在 startAsync()
    // 之前注入；运行期改分类器（目前没有 UI）不会自动重扫 —— 需要调用方 refresh()。
    // 这里刻意不读 DB：白名单已改为查询期过滤，没有“重建快照”的热路径了。
    if (m_scanning)
        m_pendingRefresh = true;
}

const FileClassifier &FileIndex::classifier() const
{
    return *m_classifier;
}

void FileIndex::setExcludeRules(const ExcludeRuleSet &rules)
{
    // 采集规则的变更代价是“重扫磁盘”，这里**刻意不自动触发** —— 由调用方决定
    // 何时 refresh（UI 需要先提示“正在重新建立索引”）。
    m_rules = rules;
}

const ExcludeRuleSet &FileIndex::excludeRules() const
{
    return m_rules;
}

void FileIndex::setKindFilter(const KindFilter &filter)
{
    if (m_kindFilter == filter)
        return;

    // 白名单是**检索期**过滤：只换一个值，下一次查询立即生效。
    // 不重建快照、不读 DB、更不扫盘 —— 这是 Phase B 验收 “切换 <50 ms” 的实现。
    m_kindFilter = filter;
    emit indexStateChanged();
}

const KindFilter &FileIndex::kindFilter() const
{
    return m_kindFilter;
}

QList<SearchResult> FileIndex::query(const QString &text, int limit)
{
    QList<SearchResult> out;

    // 门面在这里只剩"转发 + 格式转换"：查询逻辑全在 QueryEngine（Parser → 过滤 → 打分 → top-K），
    // 快照的只读接口把物理表示挡在后面。A4 之后这个函数不该再长出别的分支。
    const std::shared_ptr<const FileSnapshot> snap = m_snapshots->current();
    if (!snap || limit <= 0)
        return out;

    const QList<QueryEngine::Match> matches = m_query->query(text, limit, m_kindFilter);
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

QStringList FileIndex::pathsBySizeMtime(qint64 size, qint64 mtime, int limit) const
{
    QStringList out;
    if (limit <= 0)
        return out;

    const std::shared_ptr<const FileSnapshot> snap = m_snapshots->current();
    if (!snap)
        return out;

    for (int i = 0; i < snap->size() && out.size() < limit; ++i) {
        if (snap->sizeAt(i) == size && snap->mtimeAt(i) == mtime)
            out.append(snap->pathAt(i));
    }
    return out;
}

QStringList FileIndex::pathsByName(const QString &name, int limit) const
{
    QStringList out;
    if (limit <= 0 || name.isEmpty())
        return out;

    const std::shared_ptr<const FileSnapshot> snap = m_snapshots->current();
    if (!snap)
        return out;

    const QString wanted = name.toLower();
    for (int i = 0; i < snap->size() && out.size() < limit; ++i) {
        if (snap->lowerAt(i) == wanted)
            out.append(snap->pathAt(i));
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

    // 事件只知道路径：根序号靠前缀反查（找不到就 0，不影响查询）
    auto rootIdFor = [this](const QString &path) -> uint16_t {
        const QStringList rs = roots();
        for (int r = 0; r < rs.size(); ++r) {
            QString norm = QDir::fromNativeSeparators(rs.at(r));
            while (norm.size() > 3 && norm.endsWith(QLatin1Char('/')))
                norm.chop(1);
            if (!norm.isEmpty() && path.startsWith(norm, Qt::CaseInsensitive))
                return static_cast<uint16_t>(r + 1);
        }
        return 0;
    };

    auto rowFor = [this, &rootIdFor](const QString &path, uint8_t flags) {
        FileIndexDatabase::FileRow row;
        const QFileInfo info(path);
        row.path    = path;
        row.name    = info.fileName();
        row.size    = info.size();
        row.mtime   = info.lastModified().toSecsSinceEpoch();
        row.catMask = m_classifier->classify(row.name);
        row.flags   = flags;
        row.rootId  = rootIdFor(path);
        return row;
    };

    FileIndexDatabase::Diff diff;

    for (int i = 0; i < current->size(); ++i) {
        const QString path = current->pathAt(i);
        const QString key  = path.toLower();
        auto it = changes.constFind(key);
        if (it == changes.constEnd()) {
            const QString name = current->nameAt(i);
            uint16_t mask = current->catMaskAt(i);
            if (mask == 0)
                mask = m_classifier->classify(name);
            builder.add(name, path, current->sizeAt(i), current->mtimeAt(i),
                        mask, current->flagsAt(i), current->rootIdAt(i));
            continue;
        }

        handled.insert(key);
        if (it->type == FileSystemEvent::Removed)
            continue;                                  // 从快照里消失

        const QFileInfo info(path);
        if (!info.isFile())
            continue;                                  // 目录/已消失：不进索引

        const QString name = info.fileName();
        const uint16_t mask = m_classifier->classify(name);
        const uint8_t flags = current->flagsAt(i);
        const qint64 size = info.size();
        const qint64 mtime = info.lastModified().toSecsSinceEpoch();

        // 快照存全量；白名单在查询期判，所以这里直接加进去。
        if (it->type == FileSystemEvent::Modified)
            diff.changed.append(rowFor(path, flags));

        builder.add(name, path, size, mtime, mask, flags, current->rootIdAt(i));
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

        const QString name = info.fileName();
        const uint16_t mask = m_classifier->classify(name);
        diff.added.append(rowFor(it->path, FileFlags::None));

        builder.add(name, it->path, info.size(),
                    info.lastModified().toSecsSinceEpoch(),
                    mask, FileFlags::None, rootIdFor(it->path));
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
