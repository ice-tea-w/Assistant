#pragma once

#include "core/sync/ITransport.h"

#include <QHash>

namespace Sync {

/// 以"共享目录"为中介的传输实现。共享目录由外部网盘负责异地同步，
/// 本类只做本地文件系统读写，因此不含任何网络代码 —— 这也是
/// 单机双实例即可演示的原因（NFR5）。
///
/// 清单模型说明（与设计文档 §4.5 的差异，理由见 docs/devlog）：
/// 文档原设计是"每台设备写自己的 manifest-<deviceId>.json"。实测该模型有缺陷：
/// A 下载 B 推来的文件后不会改写自己的清单，于是 B 下次读 A 的清单会看到一个
/// 过期哈希，判定"对端又改了"，把同一份内容反复下载。
/// 因此本实现改为**单一清单 `.sync/manifest.json` 描述共享目录当前内容**，
/// 读改写后原子替换。共享目录即为对端发布态，两端都以它为 R。
class FolderTransport : public ITransport
{
public:
    /// sharedDir: 共享目录的绝对路径（其中含 .sync/ 子目录）
    explicit FolderTransport(const QString &sharedDir);
    ~FolderTransport() override;

    QString name() const override;        // "共享目录"
    int     priority() const override;    // 10（低于直连）
    bool    probe() override;
    QList<RemoteFileInfo> listRemote() override;
    bool    fetch(const QString &relPath, const QString &destPath) override;
    bool    push(const QString &localPath, const QString &relPath) override;
    bool    removeRemote(const QString &relPath) override;

    bool    tryLock(int waitMs = 0) override;
    void    unlock() override;

    /// 共享目录里是否已有清单（供界面区分"未初始化"与"空目录"）
    bool hasManifest() const;

    /// 本机设备名：写进清单的 "device" 字段，供对端显示
    void    setDeviceName(const QString &name);
    /// 清单里记录的对端设备名（无清单或老版本清单时为空）
    QString remoteDevice() const;
    /// 清单的 updatedAt（Unix 秒；无清单时为 0）
    qint64  manifestUpdatedAt() const;

private:
    QString metaDir() const;        // <sharedDir>/.sync
    QString manifestPath() const;   // <sharedDir>/.sync/manifest.json
    QString lockPath() const;       // <sharedDir>/.sync/lock.json
    QString trashDir() const;       // <sharedDir>/.sync/trash

    /// 把相对路径安全解析到共享目录之下；父目录不存在则创建。
    /// 不安全或越界返回空字符串。
    QString sharedPathFor(const QString &relPath) const;

    QHash<QString, RemoteFileInfo> readManifest() const;
    bool writeManifest(const QHash<QString, RemoteFileInfo> &files) const;
    bool updateManifestEntry(const QString &relPath, const RemoteFileInfo &info, bool removeEntry);

    /// 复制文件（覆盖已有目标，先写 .tmp 再 rename，避免半个文件被网盘同步走）
    static bool copyAtomically(const QString &src, const QString &dest);

    static QString hashOf(const QString &absPath);
    static RemoteFileInfo infoOf(const QString &relPath, const QString &absPath);

    QString m_sharedDir;
    QString m_deviceName;
    mutable QString m_remoteDevice;
    mutable qint64  m_manifestUpdatedAt = 0;
    bool    m_locked = false;
};

} // namespace Sync
