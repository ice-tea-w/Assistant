#pragma once

#include <QString>
#include <QList>
#include <QSet>
#include <QSqlDatabase>

#include <optional>

#include "core/sync/ConflictResolver.h"

/// 冲突记录（对应 conflicts 表一行）。
struct ConflictRecord
{
    int     id = -1;
    QString relPath;
    QString localHash;
    QString remoteHash;
    QString conflictCopy;   // 副本的相对路径
    qint64  detectedAt = 0;
    bool    resolved   = false;
};

/// sync.db 访问层。
///
/// 连接名由构造参数指定而非写死：QSqlDatabase 连接不可跨线程使用，
/// 因此主线程用 "sync"、工作线程用 "sync_worker"（见 §3.4）。
class SyncDatabase
{
public:
    explicit SyncDatabase(const QString &connectionName);

    bool open(const QString &filePath);
    void close();
    bool isOpen() const;

    // ---- sync_state（同步基线）----
    std::optional<Sync::FileState> baseline(const QString &relPath) const;
    bool upsertBaseline(const QString &relPath,
                        const Sync::FileState &state,
                        qint64 syncedAt,
                        const QString &direction);       // "push" | "pull"
    bool removeBaseline(const QString &relPath);
    QList<QString> allBaselinePaths() const;

    // ---- conflicts ----
    bool insertConflict(const ConflictRecord &rec);
    QList<ConflictRecord> recentConflicts(int limit) const;
    bool markResolved(int id);

    // ---- received（投递区领取记录）----
    bool isReceived(const QString &relPath) const;
    bool markReceived(const QString &relPath, const QString &hash);
    QSet<QString> receivedPaths() const;

private:
    bool createSchema();

    QString       m_connectionName;
    QSqlDatabase  m_db;
};
