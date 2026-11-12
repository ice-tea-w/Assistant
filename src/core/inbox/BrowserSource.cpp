#include "core/inbox/IDownloadSource.h"

#include <QDir>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

BrowserSource::BrowserSource(QString rootOverride)
    : m_rootOverride(std::move(rootOverride))
{
}

QString BrowserSource::downloadsDirectory()
{
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) && raw) {
        const QString path = QString::fromWCharArray(raw);
        CoTaskMemFree(raw);
        return QDir::fromNativeSeparators(path);
    }
    return QDir::fromNativeSeparators(QDir::homePath() + QStringLiteral("/Downloads"));
}

bool BrowserSource::isAvailable() const
{
    const QString dir = m_rootOverride.isEmpty() ? downloadsDirectory() : m_rootOverride;
    return !dir.isEmpty() && QDir(dir).exists();
}

QList<IDownloadSource::WatchRoot> BrowserSource::roots() const
{
    const QString dir = m_rootOverride.isEmpty() ? downloadsDirectory() : m_rootOverride;
    if (dir.isEmpty())
        return {};
    return { WatchRoot{ dir, false } };   // Downloads 本身不递归（子目录多为用户自己整理）
}

QStringList BrowserSource::partialExtensions() const
{
    // Chrome/Edge: .crdownload；Firefox: .part；其它下载器常见后缀
    return {
        QStringLiteral("crdownload"),
        QStringLiteral("part"),
        QStringLiteral("download"),
        QStringLiteral("opdownload"),
        QStringLiteral("tmp"),
        QStringLiteral("aria2"),
        QStringLiteral("!ut"),
        QStringLiteral("td"),
    };
}
