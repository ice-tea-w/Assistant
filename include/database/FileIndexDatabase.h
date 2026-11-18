#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <cstdint>

class QSqlDatabase;

/// `file_index.db` 的读写（架构文档 §3.5）。
///
/// **这是派生数据**：整个库都能从磁盘重扫重建，所以迁移失败允许删库重扫（§3.6）——
/// 但代码路径**仍必须走 `DatabaseMigrator`**，不能另开一条"没版本号、顺手建表"的路。
///
/// schema 版本：
///  - v1 = §3.5 的三张表（dirs / files / roots）+ 两个索引；
///  - v2 = 治理字段增补（`cat_mask` / `flags` / `root_id` / `origin_id`）+ `idx_files_sizemtime`
///    —— 这是迁移框架的第一个真实用例，**A3 就要跑通**，不等"以后需要再加"；
///  - v3 = `dirs.mtime`（2026-11-19）：目录 mtime 是"这棵子树有没有变"的唯一廉价判据，
///    增量扫描靠它把每次启动的全量枚举降到"每个目录一次 stat"。
///
/// ⚠️ **线程亲和**：`QSqlDatabase` 的连接属于创建它的线程，所以 `open()` / `close()`
/// 必须**在同一个线程内成对调用**。当前由扫描 worker 线程持有（见 `FileIndex.cpp`）——
/// 这样差量写库不会卡住 GUI 线程（§3.4 "GUI 线程永不阻塞"）。
/// A6 的"冷启动先载入 DB"会另开一条 GUI 线程自己的连接（WAL 支持多连接并发读）。
class FileIndexDatabase
{
public:
    struct FileRow
    {
        QString path;
        QString name;
        qint64  size  = 0;
        qint64  mtime = 0;   ///< Unix 秒
        uint16_t catMask  = 0;   ///< FileKind 位（v2 列）
        uint8_t  flags    = 0;   ///< FileFlags 位（v2 列）
        uint16_t rootId   = 0;   ///< 扫描根序号（v2 列）
        uint16_t originId = 0;   ///< 来源（Phase C 的下载 box；当前恒 0）
    };

    /// 目录行（v3）：增量扫描用它判断"这棵子树要不要重新枚举"。
    struct DirRow
    {
        QString path;
        qint64  mtime = 0;   ///< Unix 秒；0 = 还没 stamp 过（首次增量时必然判脏）
    };

    /// 三方差集（§3.5）：`diskSet` vs `dbSet`，交集内按 `(size, mtime)` 判 changed。
    struct Diff
    {
        QVector<FileRow> added;
        QVector<FileRow> changed;
        QVector<QString> removed;
        int unchanged = 0;

        int touched() const { return added.size() + changed.size() + removed.size(); }
        bool empty() const { return touched() == 0; }
    };

    explicit FileIndexDatabase(QString connectionName = QStringLiteral("file_index"));
    ~FileIndexDatabase();

    FileIndexDatabase(const FileIndexDatabase &) = delete;
    FileIndexDatabase &operator=(const FileIndexDatabase &) = delete;

    bool open(const QString &path);
    void close();
    bool isOpen() const;
    QString lastError() const;
    QString path() const { return m_path; }

    QVector<FileRow> loadAll() const;
    int rowCount() const;

    /// v3：全部目录 + 上次 stamp 的 mtime（增量扫描的输入）。
    QVector<DirRow> loadDirs() const;

    /// v3：批量写回目录 mtime（单事务）。路径不存在时插入，存在则更新。
    bool stampDirMtimes(const QHash<QString, qint64> &mtimes);

    /// roots 表里最近一次扫描时间（没有记录时返回 0）。
    /// 24h 策略靠它判断"这次启动要不要真的扫"。
    qint64 lastScanAt() const;

    /// 写入差集：**单事务 + 批量**，每 1000 条一行批（避免 WAL 无限膨胀）。
    bool applyDiff(const Diff &diff);

    /// 记录某个根的最近扫描时间（roots 表）。
    bool touchRoot(const QString &rootPath, qint64 epochSec);

    /// 库文件不可用（迁移失败/损坏）时删掉它 —— **只有派生数据允许这么做**。
    static bool removeDatabaseFile(const QString &dbPath);

    /// 计算三方差集。纯函数，不碰数据库（验收工具也用它）。
    static Diff diff(const QVector<FileRow> &diskRows, const QVector<FileRow> &dbRows);

private:
    bool ensureSchema();
    qint64 dirId(const QString &dirPath);
    void preloadDirIds();

    QString m_connectionName;
    QString m_path;
    QString m_lastError;
    QSqlDatabase *m_db = nullptr;
    QHash<QString, qint64> m_dirIds;
};
