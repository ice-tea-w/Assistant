#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace Sync {

/// 对端一个文件的元信息。hash 为空表示对端未提供哈希。
struct RemoteFileInfo
{
    QString relPath;    // 相对共享根的路径，始终使用 '/' 分隔
    qint64  size  = -1; // -1 表示文件不存在（删除标记）
    qint64  mtime = 0;  // Unix 秒
    QString hash;       // SHA-256 十六进制小写；空 = 未提供
};

/// 传输层抽象。SyncEngine 只依赖本接口，不依赖任何具体实现。
///
/// 实现必须满足：
///   1. 所有 relPath 使用 '/' 分隔，且已经过 PathGuard 校验后才可写入本地
///   2. fetch/push 失败时返回 false，且不得留下不完整的落盘文件
///   3. 不抛出异常
class ITransport
{
public:
    virtual ~ITransport() = default;

    /// 通道名，用于日志与界面显示。
    virtual QString name() const = 0;

    /// 优先级，数字小的先尝试。
    virtual int priority() const = 0;

    /// 健康探测：本通道当前是否可用。必须快速返回（建议 < 1s），不得阻塞。
    virtual bool probe() = 0;

    /// 拉取对端完整文件清单。
    virtual QList<RemoteFileInfo> listRemote() = 0;

    /// 取回对端文件到 destPath（destPath 为本地绝对路径，由调用方保证安全）。
    virtual bool fetch(const QString &relPath, const QString &destPath) = 0;

    /// 把本地文件推送到对端 relPath 位置。
    virtual bool push(const QString &localPath, const QString &relPath) = 0;

    /// 请求对端移除 relPath（对端实现应移入回收目录而不是直接删除）。
    virtual bool removeRemote(const QString &relPath) = 0;

    /// 尝试获取跨设备互斥锁，用于串行化"读清单-改-写"。
    /// 默认实现表示该通道不需要互斥。waitMs: 最多等待多久，0 = 只试一次。
    /// 成功后必须成对调用 unlock()。
    virtual bool tryLock(int waitMs = 0) { (void)waitMs; return true; }
    virtual void unlock() {}

    /// 从对端清单推断出的别名集合（relPath 的第一段），供界面提示"两边别名要一致"。
    /// 默认实现表示该通道不支持推断。
    virtual QStringList remoteAliases() const { return {}; }
};

} // namespace Sync
