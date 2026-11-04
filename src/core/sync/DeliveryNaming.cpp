#include "core/sync/DeliveryNaming.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace Sync {

namespace {

const QString &illegalChars()
{
    // '_' 也换掉：它是 "<设备>_<时间戳>_<原名>" 的分隔符，
    // 设备名里带下划线会把前缀解析搅乱。
    static const QString chars = QStringLiteral("<>:\"/\\|?*_");
    return chars;
}

/// 形如 20261015-213000
bool looksLikeStamp(const QString &s)
{
    if (s.size() != 15 || s.at(8) != QLatin1Char('-'))
        return false;
    for (int i = 0; i < s.size(); ++i) {
        if (i == 8)
            continue;
        if (!s.at(i).isDigit())
            return false;
    }
    return true;
}

} // namespace

QString sanitizeDeviceName(const QString &device)
{
    QString safe = device.trimmed().isEmpty() ? QStringLiteral("device") : device.trimmed();
    for (int i = 0; i < safe.size(); ++i) {
        if (illegalChars().contains(safe.at(i)))
            safe[i] = QLatin1Char('-');
    }
    return safe;
}

bool isOwnDelivery(const QString &deliveryName, const QString &device)
{
    return deliveryName.startsWith(sanitizeDeviceName(device) + QLatin1Char('_'));
}

QString makeDeliveryFileName(const QString &originalName, const QDateTime &stamp, int reserve)
{
    const QString stampStr = stamp.toString(QStringLiteral("yyyyMMdd-HHmmss"));

    // 总长压到 120 字符以内，超长时优先保住扩展名
    const int room = 120 - reserve - stampStr.size() - 1;
    QString original = originalName;
    if (room > 0 && original.size() > room) {
        const QFileInfo fi(originalName);
        const QString suffix = fi.suffix().isEmpty() ? QString()
                                                     : QStringLiteral(".") + fi.suffix();
        original = fi.completeBaseName().left(qMax(1, room - suffix.size())) + suffix;
    }

    return stampStr + QLatin1Char('_') + original;
}

QString makeDeliveryName(const QString &device, const QString &originalName, const QDateTime &stamp)
{
    const QString safe = sanitizeDeviceName(device);
    return safe + QLatin1Char('_') + makeDeliveryFileName(originalName, stamp, safe.size() + 1);
}

QString stripDeliveryPrefix(const QString &deliveryName)
{
    const int first = deliveryName.indexOf(QLatin1Char('_'));
    if (first <= 0)
        return deliveryName;

    // 新格式：<时间戳>_<原名>
    if (looksLikeStamp(deliveryName.left(first))) {
        const QString original = deliveryName.mid(first + 1);
        return original.isEmpty() ? deliveryName : original;
    }

    // 旧格式：<设备>_<时间戳>_<原名>
    const int second = deliveryName.indexOf(QLatin1Char('_'), first + 1);
    if (second < 0)
        return deliveryName;
    if (!looksLikeStamp(deliveryName.mid(first + 1, second - first - 1)))
        return deliveryName;

    const QString original = deliveryName.mid(second + 1);
    return original.isEmpty() ? deliveryName : original;
}

QString placeIncoming(const QString &srcPath, const QString &destDir)
{
    const QFileInfo src(srcPath);
    if (!src.isFile() || destDir.isEmpty())
        return QString();

    QDir().mkpath(destDir);

    // 落进收件箱时用原名：投递区的 "<设备>_<时间戳>_" 前缀只是中转用的
    const QString base = stripDeliveryPrefix(src.fileName());
    const QString stem = QFileInfo(base).completeBaseName();
    const QString suffix = QFileInfo(base).suffix().isEmpty()
                               ? QString()
                               : QStringLiteral(".") + QFileInfo(base).suffix();

    const QDir dir(destDir);
    QString candidate = dir.absoluteFilePath(base);
    for (int i = 2; QFileInfo::exists(candidate); ++i)
        candidate = dir.absoluteFilePath(QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(suffix));

    if (!QFile::copy(srcPath, candidate))
        return QString();
    return candidate;
}

} // namespace Sync
