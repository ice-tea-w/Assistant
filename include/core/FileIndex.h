#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <memory>

#include "ui/SearchResult.h"

class FileSnapshot;
class SnapshotManager;
class QueryEngine;
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

signals:
    /// 第一份快照（或重扫后的新快照）发布完成，在 **GUI 线程**发出。
    void indexReady(int entries, qint64 msLastScan);
    void indexStateChanged();

private:
    void onScanFinished();
    void startScan();

    std::unique_ptr<SnapshotManager> m_snapshots;
    std::unique_ptr<QueryEngine>     m_query;
    std::unique_ptr<ScanWorker>      m_worker;
    QString m_databasePath;
    bool   m_scanning       = false;
    bool   m_pendingRefresh = false;
    qint64 m_msLastScan     = 0;
    int    m_entryLimit     = 0;   ///< 0 = 用 §3.3 的默认硬上限
    int    m_dbRows         = 0;
};
