#include "core/AutoStart.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace {

/// HKCU 下的 Run 键（QSettings 的注册表路径写法）
const QString kRunKey = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");

const QString kEntryName = QStringLiteral("Assistant");

} // namespace

namespace AutoStart {

QString entryName()
{
    return kEntryName;
}

QString command()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QLatin1Char('"') + exe + QLatin1Char('"');
}

bool isEnabled()
{
    QSettings reg(kRunKey, QSettings::NativeFormat);
    const QString stored = reg.value(kEntryName).toString().trimmed();

    if (stored.isEmpty())
        return false;

    // 值存在但指向别的路径 → 视为未启用（程序被移动过，需要重新写入）
    return stored.compare(command(), Qt::CaseInsensitive) == 0;
}

bool setEnabled(bool enabled)
{
    QSettings reg(kRunKey, QSettings::NativeFormat);

    if (enabled) {
        reg.setValue(kEntryName, command());
    } else {
        reg.remove(kEntryName);
    }
    reg.sync();

    if (reg.status() != QSettings::NoError) {
        Logger::get().warn("AutoStart: registry write failed (status: {})",
                           static_cast<int>(reg.status()));
        return false;
    }

    if (enabled)
        Logger::get().info("AutoStart: enabled — {}", command().toStdString());
    else
        Logger::get().info("AutoStart: disabled");

    return true;
}

} // namespace AutoStart
