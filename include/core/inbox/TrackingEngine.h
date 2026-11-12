#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include "contract/FileSystemEvent.h"

class DownloadsDatabase;
class FileEventDispatcher;
class FileIndex;
class QTimer;
struct DownloadRow;

/// 去向追踪（治理文档 §2.6，S10）。
///
/// 解决的问题：`ReadDirectoryChangesW` 只告诉 box "文件走了"，不告诉"去了哪"。
/// 做法：入 box 时登记 `(volumeSerial, FileId)` 与 `quickSig`，用户移动后：
///  - 同卷：FileId 相同（再用 size/mtime 校验）→ `exact`；
///  - 跨卷：size/mtime 命中 + `quickSig` 相同且**唯一** → `strong`；
///  - 无法唯一确认 → `Unknown`，**绝不猜**。
///
/// 多级移动：`FiledAway` 记录的 `cur_path` 再次被移走时，重新进入追踪窗口，
/// `tracking_events` 追加一条链。
class TrackingEngine : public QObject
{
    Q_OBJECT
public:
    TrackingEngine(DownloadsDatabase *db, FileIndex *index, QObject *parent = nullptr);
    ~TrackingEngine() override;

    void setBoxPath(const QString &path);
    void setEventSource(FileEventDispatcher *dispatcher);

    /// 追踪窗口，默认 60 s（配置 `inbox.tracking.windowSec`）。测试可缩短。
    void setWindowMs(int ms);
    int  windowMs() const { return m_windowMs; }

    void start();
    void stop();
    int  pendingCount() const { return static_cast<int>(m_pending.size()); }

    /// 启动/索引就绪后的离线对账：box 内改名（级别 0）→ (size,mtime)+quickSig 反查（级别 1）
    /// → `Unknown`。公开出来便于测试与手动修复。
    void reconcileOffline();

    /// 用户手动“重新定位”一条 Unknown/已移出记录：级别 1 + 级别 2（名字 + FileId）。
    bool relocate(int downloadId, QString *error = nullptr);

signals:
    void changed();
    void notice(const QString &text);

private:
    struct Target
    {
        quint32 volumeSerial = 0;
        QString fileId;
        qint64  size = 0;
        qint64  mtime = 0;
        QString sigQuick;
    };

    struct Pending
    {
        int     downloadId = -1;
        QString fromPath;
        qint64  deadlineMs = 0;
        Target  target;
    };

    struct Candidate
    {
        QString path;
        quint32 volumeSerial = 0;
        QString fileId;
        qint64  size = 0;
        qint64  mtime = 0;
        qint64  seenMs = 0;
    };

    enum class MatchKind { None, Exact, Strong };

    void handleBatch(const QVector<FileSystemEvent> &batch);
    void onRemoved(const QString &path);
    void onRenamed(const QString &oldPath, const QString &newPath);
    void onAdded(const QString &path);

    void startTracking(int downloadId, const QString &fromPath);
    void tryCandidate(const QString &path);
    MatchKind matchKind(const Target &target, const Candidate &cand,
                        const QString &sig) const;
    void confirm(Pending &pending, const QString &toPath,
                 const QString &method, const QString &confidence);
    void markUnknown(Pending &pending, const QString &method);
    void tick();

    bool isUnderBox(const QString &path) const;
    QString normalized(const QString &path) const;

    /// 级别 1/2 的离线候选判定；成功时给出唯一路径与 confidence（strong/exact）。
    bool findOfflineCandidate(const DownloadRow &row, QString *path,
                              QString *confidence) const;

    DownloadsDatabase    *m_db = nullptr;
    FileIndex            *m_index = nullptr;
    FileEventDispatcher  *m_dispatcher = nullptr;
    int                   m_subscription = 0;
    QTimer               *m_timer = nullptr;
    QString               m_boxPath;
    int                   m_windowMs = 60000;
    bool                  m_running = false;
    bool                  m_offlineDone = false;

    QHash<int, Pending>          m_pending;
    QHash<QString, Candidate>    m_recent;   ///< 跨卷时新增事件可能先于 box 的删除到达
};
