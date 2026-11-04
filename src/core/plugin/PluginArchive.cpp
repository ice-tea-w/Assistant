#include "core/plugin/PluginArchive.h"

#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <miniz.h>

namespace {

/// Normalise an archive entry name to '/'-separated form.
QString normalise(const QString &name)
{
    QString out = name;
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out.trimmed();
}

/// Case-insensitive "is `path` inside `root`" check on already-cleaned paths.
bool isInside(const QString &root, const QString &path)
{
    const QString r = QDir::cleanPath(root);
    const QString p = QDir::cleanPath(path);

    if (p.compare(r, Qt::CaseInsensitive) == 0)
        return true;

    return p.startsWith(r + QLatin1Char('/'), Qt::CaseInsensitive);
}

/// Map an errno-ish failure onto an LLD error code.
QString codeForWriteFailure(const QString &detail)
{
    const QString lower = detail.toLower();
    if (lower.contains(QLatin1String("no space")) || lower.contains(QLatin1String("disk full")))
        return QStringLiteral("disk-full");
    return QStringLiteral("permission");
}

void removeTree(const QString &dir)
{
    if (dir.isEmpty())
        return;
    QDir(dir).removeRecursively();
}

} // namespace

// ---------------------------------------------------------------------------
// checkEntryName
// ---------------------------------------------------------------------------

QString PluginArchive::checkEntryName(const QString &entryName)
{
    const QString name = normalise(entryName);

    if (name.isEmpty())
        return QStringLiteral("条目名为空");

    // Trailing '/' marks a directory entry and is legitimate; strip it for checks.
    QString body = name;
    while (body.endsWith(QLatin1Char('/')))
        body.chop(1);

    if (body.isEmpty())
        return QStringLiteral("条目名为空");

    if (body.startsWith(QLatin1Char('/')))
        return QStringLiteral("绝对路径");

    // Drive letters (C:, D:) and NTFS alternate data streams (file:stream) both
    // contain a colon. Neither has a legitimate use in a plugin package.
    if (body.contains(QLatin1Char(':')))
        return QStringLiteral("含冒号（盘符或备用数据流）");

    const QStringList segments = body.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (segment == QLatin1String(".."))
            return QStringLiteral("包含 .. 路径段");
    }

    return QString();
}

// ---------------------------------------------------------------------------
// findPackageRoot
// ---------------------------------------------------------------------------

QString PluginArchive::findPackageRoot(const QString &extractDir)
{
    QDir dir(extractDir);
    if (!dir.exists())
        return QString();

    // Case 1: plugin.json sits at the root of the archive.
    if (QFileInfo::exists(dir.filePath(QStringLiteral("plugin.json"))))
        return dir.absolutePath();

    // Case 2: everything is wrapped in exactly one top-level directory.
    const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (subdirs.size() == 1) {
        const QString candidate = dir.filePath(subdirs.first());
        if (QFileInfo::exists(candidate + QStringLiteral("/plugin.json")))
            return QDir(candidate).absolutePath();
    }

    return QString();
}

// ---------------------------------------------------------------------------
// extract
// ---------------------------------------------------------------------------

PluginArchive::Extracted PluginArchive::extract(const QString &zipPath,
                                                const QString &targetDir)
{
    Extracted result;

    const auto fail = [&result, &targetDir](const QString &code, const QString &detail) {
        result.ok     = false;
        result.code   = code;
        result.detail = detail;
        removeTree(targetDir);        // never leave a half-extracted tree behind
        return result;
    };

    if (!QFileInfo::exists(zipPath))
        return fail(QStringLiteral("not-found"), QStringLiteral("压缩包不存在：%1").arg(zipPath));

    if (!QDir().mkpath(targetDir))
        return fail(QStringLiteral("permission"),
                    QStringLiteral("无法创建解压目录：%1").arg(targetDir));

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0))
        return fail(QStringLiteral("invalid-zip"),
                    QStringLiteral("无法读取压缩包（格式错误或已损坏）"));

    const mz_uint count = mz_zip_reader_get_num_files(&zip);

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("invalid-zip"),
                        QStringLiteral("无法读取第 %1 个条目").arg(i));
        }

        const QString rawName = QString::fromUtf8(stat.m_filename);
        const QString name    = normalise(rawName);

        // ---- security gate ----
        const QString reason = checkEntryName(rawName);
        if (!reason.isEmpty()) {
            mz_zip_reader_end(&zip);
            Logger::get().warn("PluginArchive: rejected entry '{}' — {}",
                               rawName.toStdString(), reason.toStdString());
            return fail(QStringLiteral("zip-slip"),
                        QStringLiteral("压缩包条目非法（%1）：%2").arg(reason, rawName));
        }

        const QString outPath = targetDir + QLatin1Char('/') + name;

        // Defense in depth: even after the name check, make sure the resolved path
        // is still inside the target directory.
        if (!isInside(targetDir, outPath)) {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("zip-slip"),
                        QStringLiteral("条目解析后越界：%1").arg(rawName));
        }

        if (stat.m_is_directory) {
            if (!QDir().mkpath(outPath)) {
                mz_zip_reader_end(&zip);
                return fail(QStringLiteral("permission"),
                            QStringLiteral("无法创建目录：%1").arg(name));
            }
            continue;
        }

        // Parent directory for the file.
        const QString parent = QFileInfo(outPath).absolutePath();
        if (!QDir().mkpath(parent)) {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("permission"),
                        QStringLiteral("无法创建目录：%1").arg(parent));
        }

        if (!mz_zip_reader_extract_to_file(&zip, i,
                                          outPath.toUtf8().constData(), 0)) {
            mz_zip_reader_end(&zip);
            const QString code = codeForWriteFailure(name);
            return fail(code, QStringLiteral("写入失败：%1").arg(name));
        }

        ++result.fileCount;
    }

    mz_zip_reader_end(&zip);

    result.packageRoot = findPackageRoot(targetDir);
    if (result.packageRoot.isEmpty())
        return fail(QStringLiteral("not-a-plugin"),
                    QStringLiteral("压缩包里没有 plugin.json，这不是一个 Assistant 插件包"));

    result.ok = true;
    result.code.clear();
    result.detail.clear();
    return result;
}
