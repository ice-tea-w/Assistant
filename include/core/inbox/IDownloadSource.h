#pragma once

#include <QList>
#include <QString>
#include <QStringList>

/// 下载来源适配器（治理文档 §2.4）。Phase C 只实现 `browser`（系统 Downloads）。
///
/// 后续 QQ / 微信 / 钉钉 只需新增实现：各自的目录发现、临时后缀、静默期。
class IDownloadSource
{
public:
    virtual ~IDownloadSource() = default;

    struct WatchRoot
    {
        QString dir;
        bool    recursive = true;
    };

    virtual QString sourceId() const = 0;       ///< "browser" / "wechat" ...
    virtual QString displayName() const = 0;
    virtual bool    isAvailable() const = 0;
    virtual QList<WatchRoot> roots() const = 0;
    virtual QStringList partialExtensions() const = 0;   ///< 不带点：crdownload / part ...
    virtual int     settleMs() const = 0;                ///< 稳定静默期
};

/// 系统「下载」目录。用 `SHGetKnownFolderPath(FOLDERID_Downloads)`，不硬编码
/// `%USERPROFILE%\Downloads`（用户可重定向，OneDrive 也可能接管）。
class BrowserSource : public IDownloadSource
{
public:
    /// @param rootOverride 仅测试用：绕过 known folder，指向临时目录。
    explicit BrowserSource(QString rootOverride = QString());

    QString sourceId() const override { return QStringLiteral("browser"); }
    QString displayName() const override { return QStringLiteral("浏览器下载"); }
    bool    isAvailable() const override;
    QList<WatchRoot> roots() const override;
    QStringList partialExtensions() const override;
    int     settleMs() const override { return 1200; }

    static QString downloadsDirectory();

private:
    QString m_rootOverride;
};
