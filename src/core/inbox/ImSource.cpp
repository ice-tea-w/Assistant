#include "core/inbox/ImSource.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

ImSource::ImSource(QStringList dirs)
{
    for (const QString &dir : dirs) {
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(dir.trimmed()));
        if (!clean.isEmpty() && !m_dirs.contains(clean))
            m_dirs.append(clean);
    }
}

bool ImSource::isAvailable() const
{
    for (const QString &dir : m_dirs) {
        if (QFileInfo(dir).isDir())
            return true;
    }
    return false;
}

QList<IDownloadSource::WatchRoot> ImSource::roots() const
{
    QList<WatchRoot> out;
    for (const QString &dir : m_dirs) {
        if (QFileInfo(dir).isDir())
            out.append(WatchRoot{ dir, true });
    }
    return out;
}

QStringList ImSource::partialExtensions() const
{
    return {
        QStringLiteral("tmp"),
        QStringLiteral("part"),
        QStringLiteral("download"),
        QStringLiteral("crdownload"),
        QStringLiteral("opdownload"),
        QStringLiteral("!ut"),
    };
}

QStringList ImSource::discover()
{
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString home = QDir::homePath();

    const QStringList candidates = {
        docs + QStringLiteral("/WeChat Files"),
        docs + QStringLiteral("/xwechat_files"),
        docs + QStringLiteral("/Tencent Files"),
        docs + QStringLiteral("/DingTalk"),
        docs + QStringLiteral("/QQ"),
        home + QStringLiteral("/DingTalk"),
    };

    QStringList found;
    for (const QString &candidate : candidates) {
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(candidate));
        if (QFileInfo(clean).isDir() && !found.contains(clean))
            found.append(clean);
    }
    return found;
}
