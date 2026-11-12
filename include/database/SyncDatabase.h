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

/// 已配对设备（对应 devices 表一行）。
///
/// secret 是配对时通过 SAS 通道交换的 32 字节对称口令：host 用它验客户端的
/// 挑战应答；泄露即等于可冒充该设备（设计 §4.3 的已知取舍）。
struct DeviceRecord
{
    QString    deviceId;
    QString    fingerprint;     // 对端证书的 SHA-256，小写 hex
    QByteArray secret;
    qint64     pairedAt    = 0;
    qint64     lastSeenAt  = 0;
};

/// 待发队列（对应 outbox 表一行）。
///
/// 队友不在线时发送失败，条目留在这里等下一轮；relPath 在表内唯一，
/// 同一路径重复入队算覆盖（改的是"要发的内容"，不是"发了几次"）。
struct OutboxItem
{
    qint64  id            = -1;
    QString localPath;              // 本地源文件（发送时读它）
    QString relPath;                // 对端落点，固定 `_inbox/<发送者>/<时间戳>_<原名>`
    qint64  size          = 0;
    qint64  createdAt     = 0;
    int     attempts      = 0;
    qint64  nextAttemptAt = 0;      // 早于此时间不重试
    QString lastError;
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

    // ---- devices（直连通道已配对设备）----
    bool upsertDevice(const DeviceRecord &rec);
    std::optional<DeviceRecord> device(const QString &deviceId) const;
    QList<DeviceRecord> allDevices() const;
    bool removeDevice(const QString &deviceId);
    bool touchDevice(const QString &deviceId, qint64 seenAt);

    // ---- outbox（待发队列 / 单文件直发）----
    bool enqueueOutbox(OutboxItem &item);            // 成功后回填 id
    QList<OutboxItem> dueOutbox(qint64 now) const;   // next_attempt_at <= now
    QList<OutboxItem> outboxItems() const;           // 全部，按入队顺序
    bool markOutboxSent(qint64 id);                  // 发送成功后删行
    bool markOutboxFailed(qint64 id, const QString &error, qint64 nextAttemptAt);
    bool removeOutbox(qint64 id);
    int  outboxCount() const;

private:
    bool createSchema();

    QString       m_connectionName;
    QSqlDatabase  m_db;
};
