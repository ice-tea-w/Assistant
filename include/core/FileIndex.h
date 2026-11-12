#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

#include "contract/FileSystemEvent.h"
#include "core/FileRules.h"
#include "ui/SearchResult.h"

class FileClassifier;
class FileSnapshot;
class SnapshotManager;
class QueryEngine;
class FileEventDispatcher;
class QTimer;
class ScanWorker;    // 定义在 FileIndex.cpp（内部协作者：扫描 + 差量写库）

/// 文件索引**门面**（架构文档 §2.2：薄，本方案最重要的架构约束）。
///
/// 只做四件事：持有依赖、转发调用、持有当前快照、暴露状态。
/// **不实现** DFS / SQL / 模糊匹配 / watcher —— 想给这个类加方法之前先问：
/// "这段逻辑能不能放进某个协作者？"能，就别加到门面上。
///
/// 当前边界（见 `docs/architecture/v0.4-实施清单.md` §A2–§A4）：
/// - 采集交给 `FileScanner`（Win32 迭代式 DFS），持久化交给 `FileIndexDatabase`，
///   两者都在 worker 线程里跑；门面只负责"起一次扫描 → 收快照 → 通知"。
/// - 查询转发给 `QueryEngine`（A4 起）：门面里的 `query()` 只剩格式转换。
/// - 快照仍是"扫完才发布"（A6 才做"先载入 DB 立即可查、再后台校正"）。
class FileIndex : public QObject
{
    Q_OBJECT
public:
    struct IndexStats
    {
        int    entries    = 0;      ///< 当前快照条目数
        int    dbRows     = 0;      ///< file_index.db 里的行数（0 = 没开持久化）
        bool   ready      = false;  ///< 有没有可查的快照
        bool   scanning   = false;  ///< 后台是否正在扫
        qint64 msLastScan = 0;      ///< 上次扫描耗时
        int    generation = 0;      ///< 发布过几代快照
        bool   fromCache  = false;  ///< 当前快照来自 DB 冷启动（还没被扫描校正）
    };

    explicit FileIndex(QObject *parent = nullptr);
    ~FileIndex() override;

    /// 启动索引：**立即返回**，扫描在 worker 线程进行，扫完原子发布快照。
    /// 重复调用（含扫描期间调用）安全：扫描中再调会记一个"待重扫"。
    void startAsync();

    /// 手动重建（配置变更后）。当前等价于 startAsync()。
    void refresh();

    /// 查询当前快照，返回至多 @p limit 条文件结果（best first）。
    /// 空查询/索引未就绪时返回空 —— 文件**从不**出现在空查询的默认列表里。
    /// 线程安全：快照不可变，读侧无需加锁。
    QList<SearchResult> query(const QString &text, int limit);

    /// 去向追踪的离线反查（§2.6 第六步级别 1）：按 `(size, mtime)` 精确匹配快照。
    /// 只返回路径；候选通常是个位数（走 `idx_files_sizemtime` 的等价内存扫描）。
    QStringList pathsBySizeMtime(qint64 size, qint64 mtime, int limit = 20) const;

    /// 级别 2：按文件名反查（大小写不敏感），再由调用方取 FileId 确认。
    QStringList pathsByName(const QString &name, int limit = 50) const;

    IndexStats stats() const;
    bool isReady() const;

    /// 持久化数据库路径（`file_index.db`）。传空字符串 = 不持久化（验收工具用）。
    ///
    /// 打开/关闭都在 worker 线程内成对发生 —— `QSqlDatabase` 的连接是线程亲和的，
    /// 这样差量写库不会卡 GUI 线程。
    void setDatabasePath(const QString &path);

    /// 硬保护上限（架构 §3.3），默认 100 万条；`<= 0` 表示回到默认。
    ///
    /// 生产值由 `fileIndex.hardLimitEntries` 注入。另一个用途是**验收**：
    /// 把上限压到 20000，v2 与旧引擎就落在同一候选集上，结果应当逐条一致
    /// （见 `tests/golden_file_index.cpp`）。
    void setEntryLimit(int maxEntries);

    /// 用一份现成快照替换当前索引。
    /// 增量补丁会用它；golden 测试也用它注入合成语料（不必碰磁盘）。
    void setSnapshot(std::shared_ptr<const FileSnapshot> snapshot);

    /// 扫描根目录；传空表示沿用 `FileScanner::defaultRoots()`。
    /// 必须在 `startAsync()` 之前设置（A5b 起测试与将来的配置项都走它）。
    void setRoots(const QStringList &roots);
    QStringList roots() const;

    /// A5b：接上增量事件源。传 nullptr 断开。重复设置会先断开旧的订阅。
    /// 事件在 GUI 线程回调进来，补丁计算在一次性工作线程里做（不阻塞界面）。
    void setEventSource(FileEventDispatcher *dispatcher);

    // ---- Phase B：治理（种类过滤 + 排除规则 + 工程识别）----
    //
    // 两个概念不要混（治理文档 §1.1）：
    //  - `ExcludeRuleSet` 是**采集规则**：改了必须重扫（`refresh()`）；
    //  - `KindFilter` 是**检索过滤**：改了只需从 DB 重建快照（不扫盘）。
    // 三者都必须在 `startAsync()` 之前注入时最省事；运行期调用也安全。

    /// 种类归类器（扩展名表 + 用户覆盖）。传副本 —— 扫描 worker 会拿到自己的拷贝。
    void setClassifier(const FileClassifier &classifier);
    const FileClassifier &classifier() const;

    /// 采集排除规则（目录名 / 通配 / 大小 / 工程哨兵）。**不自动重扫**：
    /// 调用方在配置变更后应调用 `refresh()`（重扫是慢路径，UI 必须提示）。
    void setExcludeRules(const ExcludeRuleSet &rules);
    const ExcludeRuleSet &excludeRules() const;

    /// 检索过滤（白名单/黑名单）。**查询期生效**：只换一个值，下一次查询立即应用，
    /// 不重建快照、不读 DB、不扫盘（Phase B 验收：切换 <50 ms）。
    void setKindFilter(const KindFilter &filter);
    const KindFilter &kindFilter() const;

signals:
    /// 第一份快照（或重扫后的新快照）发布完成，在 **GUI 线程**发出。
    void indexReady(int entries, qint64 msLastScan);

    /// A6：冷启动的"先可用"阶段 —— 快照已从 DB 载入并发布（此时扫描还没结束）。
    /// 消费者可以立刻查询；扫描完成后会再发一次 `indexReady`。
    void cacheLoaded(int entries, qint64 ms);

    void indexStateChanged();

private:
    void onScanFinished();
    void startScan();

    /// 在 GUI 线程收批；必要时抛给一次性线程做补丁。
    void startColdLoad();
    void applyEventBatch(const QVector<FileSystemEvent> &batch);
    /// 工作线程里执行：旧快照 + 事件 → 新快照 + 库差量。
    void patchSnapshotInWorker(QVector<FileSystemEvent> batch);
    void publishPatchedSnapshot(std::shared_ptr<const FileSnapshot> snapshot, int changedEvents);

    std::unique_ptr<SnapshotManager> m_snapshots;
    std::unique_ptr<QueryEngine>     m_query;
    std::unique_ptr<ScanWorker>      m_worker;
    std::unique_ptr<FileClassifier>  m_classifier;
    ExcludeRuleSet                   m_rules;
    KindFilter                       m_kindFilter;
    QString m_databasePath;
    bool   m_scanning       = false;
    bool   m_pendingRefresh = false;
    qint64 m_msLastScan     = 0;
    int    m_entryLimit     = 0;   ///< 0 = 用 §3.3 的默认硬上限
    int    m_dbRows         = 0;

    // ---- A5b 增量 ----
    QStringList                m_roots;
    FileEventDispatcher       *m_dispatcher = nullptr;
    int                        m_subscription = 0;
    std::atomic<bool>          m_patchBusy{false};
    bool                       m_fromCache = false;
    qint64                     m_coldMs    = 0;
    QVector<FileSystemEvent>   m_pendingEvents;   ///< GUI 线程
    QTimer                    *m_eventTimer = nullptr;
};
