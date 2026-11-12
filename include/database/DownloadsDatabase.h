#pragma once

#include <QList>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <cstdint>
#include <optional>

class QSqlDatabase;

/// 一条下载记录（治理文档 §2.11）。`downloads` 表的一行。
///
/// 两个状态字段**必须分开**（§2.11 建模要点）：
///  - `pipeState`：我们的一次性管线（Detected/Stabilizing/Fetching/InBox/Pending/Failed/Skipped/Undone）；
///  - `trackState`：文件持续位置状态（InBox/Tracking/FiledAway/Unknown/Deleted）。
struct DownloadRow
{
    int      id = -1;
    QString  sourceId;
    QString  srcPath;
    QString  boxPath;
    QString  curPath;
    bool     inBox = false;
    QString  name;
    QString  ext;
    uint16_t catMask = 0;
    qint64   size = 0;
    qint64   mtime = 0;
    quint32  volumeSerial = 0;
    QString  fileId;
    QString  sigQuick;
    QString  sigFull;
    QString  url;
    QString  referrer;
    QString  pipeState = QStringLiteral("Detected");
    QString  trackState = QStringLiteral("InBox");
    QString  detectedBy = QStringLiteral("watcher");
    int      dupOf = 0;
    int      errorCode = 0;
    QString  tags;
    QString  suggestDest;   ///< 规则建议去向（v2）
    QString  ruleName;      ///< 命中的规则名（v2）
    QString  createdAt;
    QString  completedAt;
    QString  fetchedAt;
    QString  filedAt;
    QString  lastVerified;
};

/// `move_journal` 一行：撤销与崩溃对账的唯一依据（**只记录我方动作**）。
struct JournalRow
{
    qint64  id = -1;
    int     downloadId = -1;
    QString batchId;
    QString op;         ///< fetch / rename / organize / trash / undo
    QString fromPath;
    QString toPath;
    QString opState;    ///< intent / done / failed
    int     errorCode = 0;
    QString at;
};

/// `downloads.db`（独立连接 `downloads`，**权威数据**：迁移失败必须拒绝启动，绝不静默重建）。
class DownloadsDatabase
{
public:
    explicit DownloadsDatabase(QString connectionName = QStringLiteral("downloads"));
    ~DownloadsDatabase();

    DownloadsDatabase(const DownloadsDatabase &) = delete;
    DownloadsDatabase &operator=(const DownloadsDatabase &) = delete;

    bool open(const QString &filePath);
    void close();
    bool isOpen() const;
    QString lastError() const { return m_lastError; }
    QString path() const { return m_path; }

    // ---- downloads ----
    int  insert(const DownloadRow &row);
    bool updateAfterFetch(int id, const QString &boxPath, qint64 size, qint64 mtime,
                          quint32 volumeSerial, const QString &fileId, const QString &quickSig);
    bool updatePipeState(int id, const QString &state, int errorCode = 0);
    bool updateTrackState(int id, const QString &state, const QString &curPath,
                          bool inBox, const QString &filedAt = QString());
    bool updatePaths(int id, const QString &srcPath, const QString &curPath);
    bool updateBoxPath(int id, const QString &boxPath);
    bool updateAfterArchive(int id, const QString &curPath);   ///< 我方归档（规则 move/手动）
    bool setSuggestion(int id, const QString &dest, const QString &ruleName);
    bool setDupOf(int id, int dupOf);

    std::optional<DownloadRow> byId(int id) const;
    std::optional<DownloadRow> bySrcPath(const QString &srcPath) const;
    std::optional<DownloadRow> byBoxPath(const QString &boxPath) const;
    /// 多级移动链：当前路径被移走时用它找回记录。
    std::optional<DownloadRow> byCurPath(const QString &curPath) const;
    /// 查重：同 quickSig 的**更早**一条记录（排除自身）。
    std::optional<DownloadRow> bySigQuick(const QString &sigQuick, int excludeId) const;
    QVector<DownloadRow> list(int limit = 200) const;
    QVector<DownloadRow> listInBox() const;
    int countInBox() const;
    int countAll() const;

    // ---- move_journal ----
    qint64 beginJournal(int downloadId, const QString &op,
                        const QString &fromPath, const QString &toPath);
    bool finishJournal(qint64 journalId, bool ok, int errorCode = 0);
    QVector<JournalRow> pendingIntents() const;
    QVector<JournalRow> journalFor(int downloadId) const;

    // ---- tracking_events（追加型去向链）----
    qint64 addTrackingEvent(int downloadId, const QString &fromPath, const QString &toPath,
                            const QString &method, const QString &confidence);
    QVector<QStringList> trackingFor(int downloadId) const;   ///< {from,to,method,confidence,at}

    // ---- app_meta ----
    QString meta(const QString &key) const;
    bool setMeta(const QString &key, const QString &value);

    // ---- sources ----
    struct SourceRow
    {
        QString sourceId;
        bool    enabled = false;
        QString pathsJson;
        QString lastSeen;
    };
    std::optional<SourceRow> source(const QString &sourceId) const;
    bool upsertSource(const SourceRow &row);

private:
    bool ensureSchema();
    bool exec(QSqlDatabase &db, const QString &sql, const char *what);

    QString m_connectionName;
    QString m_path;
    QString m_lastError;
    QSqlDatabase *m_db = nullptr;
};
