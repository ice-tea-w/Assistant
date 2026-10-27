#include "core/ConfigManager.h"
#include "core/AutoStart.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSysInfo>

// nlohmann/json — use the single-header include
// (vcpkg installs into include/nlohmann/json.hpp)
#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace {

// 同机双实例共用一个主机名，默认设备名要带实例后缀，否则投递文件名前缀撞车，
// 两边都会把对方投的文件当成自己发的而跳过领取。
QString defaultDeviceName()
{
    QString name = QSysInfo::machineHostName();
    const QString prefix = QStringLiteral("Assistant-");
    const QString appName = QCoreApplication::applicationName();
    if (appName.startsWith(prefix))
        name += QLatin1Char('-') + appName.mid(prefix.size());
    return name;
}

}

ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent)
{
    applyDefaults();
}

// ---- file path ----

QString ConfigManager::filePath() const
{
    return m_filePath;
}

QString ConfigManager::configFilePath() const
{
    // %LOCALAPPDATA%/Assistant/config.json
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = QDir::currentPath() + "/config";

    QDir().mkpath(dir);                     // ensure directory exists
    return dir + "/config.json";
}

// ---- defaults ----

void ConfigManager::applyDefaults()
{
    m_hotkeyModifiers = "Alt";
    m_hotkeyKey        = "Space";
    m_theme            = "dark";
    m_windowX          = -1;   // -1 means "not saved — let the caller decide"
    m_windowY          = -1;
    m_logLevel         = "info";

    // 设备同步：路径留空由用户在设置页填；设备名默认取主机名
    m_syncLocalRoot.clear();
    m_syncRoots.clear();
    m_syncSharedDir.clear();
    m_syncPeerAddress.clear();
    m_syncPort = 45871;
    m_syncDeviceName = defaultDeviceName();
    m_syncAutoIntervalMinutes = 0;
    m_syncWatchFiles = true;
    m_syncInboxDir.clear();
}

// ---- load ----

void ConfigManager::load()
{
    m_filePath = configFilePath();

    // 开机启动的真实来源是注册表，每次启动同步一次（不读 JSON）
    m_autoStart = AutoStart::isEnabled();

    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::get().info("ConfigManager: no config file, using defaults ({})",
                            m_filePath.toStdString());
        save();   // write defaults so the file exists next time
        emit configLoaded();
        return;
    }

    QByteArray raw = file.readAll();
    file.close();

    try {
        json j = json::parse(raw.toStdString());

        // hotkey
        if (j.contains("hotkey")) {
            auto &hk = j["hotkey"];
            if (hk.contains("modifiers")) m_hotkeyModifiers = QString::fromStdString(hk["modifiers"]);
            if (hk.contains("key"))       m_hotkeyKey        = QString::fromStdString(hk["key"]);
        }

        // window
        if (j.contains("window")) {
            auto &w = j["window"];
            if (w.contains("x")) m_windowX = w["x"];
            if (w.contains("y")) m_windowY = w["y"];
        }

        // theme
        if (j.contains("theme")) {
            m_theme = QString::fromStdString(j["theme"]);
        }

        // logging
        if (j.contains("logging")) {
            auto &log = j["logging"];
            if (log.contains("level")) m_logLevel = QString::fromStdString(log["level"]);
        }

        // sync（设备同步）
        if (j.contains("sync")) {
            auto &s = j["sync"];
            if (s.contains("localRoot"))  m_syncLocalRoot  = QString::fromStdString(s["localRoot"]);
            if (s.contains("roots") && s["roots"].is_array()) {
                m_syncRoots.clear();
                for (const auto &item : s["roots"]) {
                    if (!item.is_object())
                        continue;
                    QVariantMap one;
                    one.insert("localPath", QString::fromStdString(item.value("localPath", "")));
                    one.insert("alias",     QString::fromStdString(item.value("alias", "")));
                    if (!one.value("localPath").toString().isEmpty())
                        m_syncRoots.append(one);
                }
            }
            if (s.contains("sharedDir"))  m_syncSharedDir  = QString::fromStdString(s["sharedDir"]);
            if (s.contains("peerAddress")) m_syncPeerAddress = QString::fromStdString(s["peerAddress"]);
            if (s.contains("port"))       m_syncPort       = s["port"];
            if (s.contains("deviceName")) m_syncDeviceName = QString::fromStdString(s["deviceName"]);
            if (s.contains("autoIntervalMinutes")) m_syncAutoIntervalMinutes = s["autoIntervalMinutes"];
            if (s.contains("watchFiles"))  m_syncWatchFiles  = s["watchFiles"];
            if (s.contains("inboxDir"))    m_syncInboxDir    = QString::fromStdString(s["inboxDir"]);
        }
        if (m_syncDeviceName.isEmpty())
            m_syncDeviceName = defaultDeviceName();

        Logger::get().info("ConfigManager: loaded {}", m_filePath.toStdString());
    } catch (const std::exception &e) {
        Logger::get().warn("ConfigManager: parse error — {} — using defaults", e.what());
        applyDefaults();
        save();   // overwrite broken file
    }

    emit configLoaded();
}

// ---- save ----

void ConfigManager::save()
{
    if (m_filePath.isEmpty())
        m_filePath = configFilePath();

    json j;
    j["hotkey"] = {
        {"modifiers", m_hotkeyModifiers.toStdString()},
        {"key",       m_hotkeyKey.toStdString()}
    };
    j["window"] = {
        {"x", m_windowX},
        {"y", m_windowY}
    };
    j["theme"] = m_theme.toStdString();
    j["logging"] = {
        {"level", m_logLevel.toStdString()}
    };
    j["sync"] = {
        {"localRoot",  m_syncLocalRoot.toStdString()},
        {"sharedDir",  m_syncSharedDir.toStdString()},
        {"peerAddress", m_syncPeerAddress.toStdString()},
        {"port",       m_syncPort},
        {"deviceName", m_syncDeviceName.toStdString()},
        {"autoIntervalMinutes", m_syncAutoIntervalMinutes},
        {"watchFiles", m_syncWatchFiles},
        {"inboxDir",   m_syncInboxDir.toStdString()}
    };
    j["sync"]["roots"] = json::array();
    for (const QVariant &v : m_syncRoots) {
        const QVariantMap m = v.toMap();
        j["sync"]["roots"].push_back({
            {"localPath", m.value("localPath").toString().toStdString()},
            {"alias",     m.value("alias").toString().toStdString()}
        });
    }

    QFile file(m_filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::string s = j.dump(4);          // 4-space indent
        file.write(s.c_str(), static_cast<qint64>(s.size()));
        file.close();
        Logger::get().info("ConfigManager: saved {}", m_filePath.toStdString());
    } else {
        Logger::get().warn("ConfigManager: cannot write {}", m_filePath.toStdString());
    }
}

// ---- getters ----

QString ConfigManager::theme() const          { return m_theme; }
int     ConfigManager::windowX() const        { return m_windowX; }
int     ConfigManager::windowY() const        { return m_windowY; }
QString ConfigManager::hotkeyModifiers() const { return m_hotkeyModifiers; }
QString ConfigManager::hotkeyKey() const       { return m_hotkeyKey; }
QString ConfigManager::logLevel() const       { return m_logLevel; }
bool    ConfigManager::autoStart() const      { return m_autoStart; }

QString ConfigManager::syncLocalRoot() const  { return m_syncLocalRoot; }
QVariantList ConfigManager::syncRoots() const { return m_syncRoots; }
QString ConfigManager::syncSharedDir() const  { return m_syncSharedDir; }
QString ConfigManager::syncPeerAddress() const { return m_syncPeerAddress; }
int     ConfigManager::syncPort() const       { return m_syncPort; }
QString ConfigManager::syncDeviceName() const { return m_syncDeviceName; }
int     ConfigManager::syncAutoIntervalMinutes() const { return m_syncAutoIntervalMinutes; }
bool    ConfigManager::syncWatchFiles() const { return m_syncWatchFiles; }
QString ConfigManager::syncInboxDir() const { return m_syncInboxDir; }

// ---- setters ----

void ConfigManager::setTheme(const QString &theme)
{
    if (m_theme != theme) {
        m_theme = theme;
        save();
        emit themeChanged();
    }
}

void ConfigManager::saveWindowPosition(int x, int y)
{
    if (m_windowX != x || m_windowY != y) {
        m_windowX = x;
        m_windowY = y;
        save();
        emit windowPositionChanged();
    }
}

void ConfigManager::setHotkey(const QString &modifiers, const QString &key)
{
    if (m_hotkeyModifiers != modifiers || m_hotkeyKey != key) {
        m_hotkeyModifiers = modifiers;
        m_hotkeyKey = key;
        save();
        emit hotkeyChanged();
    }
}

void ConfigManager::setLogLevel(const QString &level)
{
    if (m_logLevel != level) {
        m_logLevel = level;
        save();
        emit logLevelChanged();
    }
}

void ConfigManager::setAutoStart(bool enabled)
{
    if (m_autoStart == enabled)
        return;

    // 注册表写失败就不改内存状态，保证 UI 与系统实际状态一致
    if (!AutoStart::setEnabled(enabled))
        return;

    m_autoStart = enabled;
    emit autoStartChanged();
}

// ---- 设备同步 setters ----

void ConfigManager::setSyncLocalRoot(const QString &path)
{
    if (m_syncLocalRoot == path)
        return;
    m_syncLocalRoot = path;
    save();
    emit syncChanged();
}

void ConfigManager::setSyncRoots(const QVariantList &roots)
{
    if (m_syncRoots == roots)
        return;
    m_syncRoots = roots;
    save();
    emit syncChanged();
}

void ConfigManager::setSyncSharedDir(const QString &path)
{
    if (m_syncSharedDir == path)
        return;
    m_syncSharedDir = path;
    save();
    emit syncChanged();
}

void ConfigManager::setSyncPeerAddress(const QString &address)
{
    if (m_syncPeerAddress == address)
        return;
    m_syncPeerAddress = address;
    save();
    emit syncChanged();
}

void ConfigManager::setSyncPort(int port)
{
    if (m_syncPort == port)
        return;
    m_syncPort = port;
    save();
    emit syncChanged();
}

void ConfigManager::setSyncDeviceName(const QString &name)
{
    if (m_syncDeviceName == name)
        return;
    m_syncDeviceName = name;
    save();
    emit syncChanged();
}

void ConfigManager::setSyncAutoIntervalMinutes(int minutes)
{
    if (m_syncAutoIntervalMinutes == minutes)
        return;
    m_syncAutoIntervalMinutes = minutes;
    save();
    emit syncChanged();
}

void ConfigManager::setSyncWatchFiles(bool enabled)
{
    if (m_syncWatchFiles == enabled)
        return;
    m_syncWatchFiles = enabled;
    save();
    emit syncChanged();
}

void ConfigManager::setSyncInboxDir(const QString &path)
{
    if (m_syncInboxDir == path)
        return;
    m_syncInboxDir = path;
    save();
    emit syncChanged();
}
