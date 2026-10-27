#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <QHash>

#include <memory>

#include "core/sync/ITransport.h"
#include "core/sync/ConflictResolver.h"

class SyncDatabase;

namespace Sync {

/// 一次同步的汇总结果。
struct SyncResult
{
    enum Status {
        Ok,                   // 全部处理完成
        NoChannelAvailable,   // 所有通道 probe() 均失败
        RemoteUnreachable,    // 通道可用但对端清单拉取失败
        Busy,                 // 另一台设备正在同步（跨设备锁被占）
        Aborted               // 用户取消或遇到致命错误
    };

    Status  status     = Ok;
    int     uploaded   = 0;
    int     downloaded = 0;
    int     conflicts  = 0;
    int     deleted    = 0;
    int     failed     = 0;   // 单个文件失败计数，不影响整体完成
    qint64  bytesMoved = 0;
    QString message;          // 失败原因，供界面显示（status != Ok 时非空）
};

/// 一个同步项：本地某个文件夹 → 共享目录里的 <别名>/ 子树。
///
/// 为什么要别名：共享目录是单一的，不同本地文件夹里的同名文件（两个都有
/// 「报告.docx」）必须落在不同位置，否则会互相覆盖。别名就是那层命名空间。
struct SyncRoot
{
    QString localPath;   // 本地文件夹绝对路径
    QString alias;       // 共享目录下的单层目录名，不含 '/' 与 '\\'
};

/// 同步引擎。所有磁盘与网络 I/O 都在本类内完成。
/// 不依赖任何 QML 类型，可脱离界面单独测试。
class SyncEngine : public QObject
{
    Q_OBJECT
public:
    explicit SyncEngine(QObject *parent = nullptr);

    void setDatabase(SyncDatabase *db);        // 不接管所有权
    /// 全部要同步的本地文件夹。别名非法或重复的项会被忽略。
    void setRoots(const QList<SyncRoot> &roots);
    void setLocalRoot(const QString &root);    // 便捷：只同步一个文件夹，别名取目录名
    void setDeviceName(const QString &name);   // 用于冲突副本命名

    /// 传入全部候选通道。sync() 内部按 priority() 升序依次 probe()，
    /// 取第一个可用者执行本次同步（见 §3.3）。
    void setTransports(const QList<std::shared_ptr<ITransport>> &list);

    /// 执行一次完整同步。**阻塞调用**，应在工作线程中执行（见 §3.4）。
    SyncResult sync();

signals:
    /// percent: 0-100；currentFile: 正在处理的相对路径
    void progress(int percent, const QString &currentFile);
    /// action: "upload" | "download" | "conflict" | "delete" | "skip"
    void fileHandled(const QString &relPath, const QString &action);

private:
    QHash<QString, FileState> scanLocal() const;
    QString localAbsFor(const QString &relPath) const;   // relPath 带 "<别名>/" 前缀
    QString rootOfAlias(const QString &alias) const;
    bool    moveToLocalTrash(const QString &absPath) const;
    static QString localTrashDir();

    SyncDatabase *m_db = nullptr;
    QList<SyncRoot> m_roots;
    QString       m_deviceName;
    QList<std::shared_ptr<ITransport>> m_transports;
};

} // namespace Sync
