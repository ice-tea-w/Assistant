#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include "contract/FileSystemEvent.h"
#include "core/inbox/DownloadRules.h"

class DownloadsDatabase;
class FileEventDispatcher;
class IDownloadSource;
class QTimer;

/// 下载接管管线（治理文档 §2.3 / §2.5 / §2.7，Phase C 的 S9 骨架）。
///
/// 职责边界：
///  - **只做**"发现下载 → A/B/C 完成度判定 → 拉入 box → 登记 → 通知"；
///  - **不做**去向追踪（Phase D）、不做归档规则/查重（Phase E）、不碰 IM；
///  - 从不删除用户文件：拉取失败一律保持源文件原位，只把状态写成 `Failed/Pending`。
///
/// 线程：所有 DB 与文件操作都在创建它的线程（GUI）执行。跨卷大文件的
/// `MoveFileExW` 可能阻塞数百毫秒 —— Phase C 的已知限制，见 devlog。
class IngestEngine : public QObject
{
    Q_OBJECT
public:
    explicit IngestEngine(DownloadsDatabase *db, QObject *parent = nullptr);
    ~IngestEngine() override;

    void setSource(const IDownloadSource *source);
    void setBoxPath(const QString &path);
    QString boxPath() const { return m_boxPath; }

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    /// 覆盖适配器的 settleMs（测试用；<=0 用适配器的值）。
    void setSettleMs(int ms) { m_settleOverrideMs = ms; }

    /// 订阅文件事件（与 `FileIndex` 共用同一个 dispatcher，互不认识）。
    void setEventSource(FileEventDispatcher *dispatcher);

    /// 启动：先做启动双对账（journal intent + box 幽灵条目），再起 400 ms 判定定时器。
    bool start();
    void stop();
    bool isRunning() const;

    /// box 幽灵条目是否由本引擎直接标 `Unknown`。
    /// 装了 `TrackingEngine` 时置 false —— 它要先尝试离线反查再下结论。
    void setAutoReconcileBox(bool enabled) { m_autoReconcileBox = enabled; }

    /// 存档规则（S11）。默认只给建议；`autoOrganize` 开启后 `move` 才执行。
    void setRules(const DownloadRules &rules) { m_rules = rules; }
    void setAutoOrganize(bool enabled) { m_autoOrganize = enabled; }

    /// 按建议/指定目录归档（把文件从 box 移到 dest 目录，走 journal）。
    bool archive(int downloadId, const QString &destDir, QString *error = nullptr);

    /// 撤销**本应用**的归档：journal 里最后一次 organize 必须成功且目标仍是 cur_path；
    /// 不是本应用移动的（用户自己移走）不动。
    bool unarchive(int downloadId, QString *error = nullptr);

    /// 撤销一次接管：把文件从 box 移回原下载目录（同名冲突自动加 (n)）。
    bool undo(int downloadId, QString *error = nullptr);

    /// 立即做一次对账；返回处理的记录数（测试与手动修复用）。
    int reconcile();

    QString lastNotice() const { return m_lastNotice; }

signals:
    /// 成功拉入 box（托盘通知与 UI 刷新都接这里）。
    void captured(int downloadId, const QString &name, qint64 size);
    void changed();
    void notice(const QString &text);

private:
    struct Candidate
    {
        QString path;
        qint64  size = 0;
        qint64  mtime = 0;
        qint64  firstSeenMs = 0;
        qint64  lastChangeMs = 0;
        qint64  nextTryMs = 0;
        int     attempts = 0;
        bool    abOnly = false;   ///< C 重试用尽，按 A+B 结论接收
    };

    void handleBatch(const QVector<FileSystemEvent> &batch);
    void considerPath(const QString &path);
    void removeCandidate(const QString &path);
    void tick();
    void admit(const Candidate &candidate);
    void registerCaptured(int id, const QString &path);

    bool isUnderSource(const QString &path) const;
    bool hasPartialExtension(const QString &name) const;
    int  effectiveSettleMs() const;
    QString uniqueTarget(const QString &dir, const QString &fileName) const;
    bool    ensureBox(QString *error) const;

    void reconcileJournal(QStringList *notes);
    void reconcileBox(QStringList *notes);
    void setNotice(const QString &text);

    DownloadsDatabase  *m_db = nullptr;
    const IDownloadSource *m_source = nullptr;
    FileEventDispatcher *m_dispatcher = nullptr;
    int                  m_subscription = 0;
    QTimer              *m_timer = nullptr;
    QString              m_boxPath;
    bool                 m_enabled = false;
    bool                 m_running = false;
    bool                 m_autoReconcileBox = true;
    DownloadRules        m_rules;
    bool                 m_autoOrganize = false;
    int                  m_settleOverrideMs = 0;
    QString              m_lastNotice;

    QHash<QString, Candidate> m_candidates;   ///< key = lower(path)
};
