#include "core/sync/PathGuard.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStringList>

namespace Sync::PathGuard {

namespace {

/// Windows 保留设备名（不区分大小写，且带扩展名也算，如 "CON.txt"）。
const QSet<QString> &reservedNames()
{
    static const QSet<QString> s = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")
    };
    return s;
}

} // namespace

bool isSafeRelativePath(const QString &relPath)
{
    if (relPath.isEmpty())
        return false;

    // 绝对路径与盘符（"C:foo" 这种驱动器相对路径同样危险）
    if (QDir::isAbsolutePath(relPath))
        return false;
    if (relPath.length() >= 2 && relPath.at(1) == QLatin1Char(':'))
        return false;

    // UNC 形式
    if (relPath.startsWith(QStringLiteral("\\\\")) || relPath.startsWith(QStringLiteral("//")))
        return false;

    // 控制字符（含换行、制表）—— 可用于欺骗日志与界面
    for (const QChar &c : relPath) {
        if (c.unicode() < 0x20)
            return false;
    }

    // NTFS 备用数据流：文件名中的冒号
    if (relPath.contains(QLatin1Char(':')))
        return false;

    const QString normalized = QString(relPath).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList segments = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    if (segments.isEmpty())
        return false;

    for (const QString &seg : segments) {
        if (seg == QStringLiteral(".") || seg == QStringLiteral(".."))
            return false;
        if (seg.endsWith(QLatin1Char('.')) || seg.endsWith(QLatin1Char(' ')))
            return false;   // Windows 会静默去掉尾部的点与空格，可用于绕过检查
        if (seg.length() > 255)
            return false;

        const QString stem = seg.section(QLatin1Char('.'), 0, 0).toUpper();
        if (reservedNames().contains(stem))
            return false;
    }

    return true;
}

QString resolveUnder(const QString &root, const QString &relPath)
{
    if (!isSafeRelativePath(relPath))
        return QString();

    const QString normalized = QString(relPath).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString rootAbs = QDir(root).absolutePath();
    const QString joined  = QDir(rootAbs).absoluteFilePath(normalized);

    // 关键一步：解引用符号链接与 junction 后再复核前缀。
    // 只做字符串前缀比较是不够的 —— 共享目录里可能存在指向外部目录的 junction。
    // 项目已有同类踩坑记录：docs/troubleshooting/qt-nosymlinks-挡不住junction.md
    const QFileInfo fi(joined);
    const QString canonicalParent = fi.dir().canonicalPath();
    if (canonicalParent.isEmpty())
        return QString();       // 父目录不存在或无法解析

    const QString canonicalRoot = QFileInfo(rootAbs).canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return QString();

    const QString canonicalFull = QDir(canonicalParent).absoluteFilePath(fi.fileName());

    const QString rootWithSep = canonicalRoot.endsWith(QLatin1Char('/'))
                                    ? canonicalRoot
                                    : canonicalRoot + QLatin1Char('/');
    if (canonicalFull != canonicalRoot && !canonicalFull.startsWith(rootWithSep, Qt::CaseInsensitive))
        return QString();

    return canonicalFull;
}

} // namespace Sync::PathGuard
