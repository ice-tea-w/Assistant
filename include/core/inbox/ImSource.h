#pragma once

#include "core/inbox/IDownloadSource.h"

/// IM 接收目录适配器（治理文档 §2.4）。
///
/// 不做“猜目录 + 自动接管”：目录来自用户显式配置（`inbox.im.dirs`），默认关闭。
/// 一个适配器覆盖 QQ / 微信 / 钉钉，`sourceId` 统一为 `im`；每个目录递归监听，
/// 临时后缀沿用浏览器那一套并补上 IM 常见的 `.tmp`。
class ImSource : public IDownloadSource
{
public:
    explicit ImSource(QStringList dirs);

    QString sourceId() const override { return QStringLiteral("im"); }
    QString displayName() const override { return QStringLiteral("IM 接收文件"); }
    bool    isAvailable() const override;
    QList<WatchRoot> roots() const override;
    QStringList partialExtensions() const override;
    int     settleMs() const override { return 2000; }

    QStringList dirs() const { return m_dirs; }

    /// 常见安装位置（只返回**当前存在**的目录），用于设置页“扫描常见目录”。
    static QStringList discover();

private:
    QStringList m_dirs;
};
