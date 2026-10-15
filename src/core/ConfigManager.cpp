#include "core/ConfigManager.h"
#include "core/AutoStart.h"
#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

// nlohmann/json — use the single-header include
// (vcpkg installs into include/nlohmann/json.hpp)
#include <nlohmann/json.hpp>
using json = nlohmann::json;

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
    // A2 已通过验收（见 docs/devlog/2026-10-16.md），默认切 v2。
    // 真机实测：v2 索引 31810 条 vs legacy 的 20000 条上限，而单遍全表扫描
    // 326 ms vs 333 ms —— 多收 1.2 万个文件（Documents/Downloads 终于进得来了）
    // 没有换来查询变慢。要退回去就把这里或 config.json 的 fileIndex.engine
    // 改成 "legacy"，旧 FileSearcher 在 A7 之前一直保留。
    m_fileIndexEngine  = "v2";
}

/// 只认 "legacy" / "v2" 两个值；写错（或旧版 config 里没有这一节）都按 legacy。
QString ConfigManager::normalizedEngine(const QString &raw)
{
    return raw.compare(QStringLiteral("v2"), Qt::CaseInsensitive) == 0
               ? QStringLiteral("v2")
               : QStringLiteral("legacy");
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

        // file index engine (A2 的 A/B 开关)
        if (j.contains("fileIndex")) {
            auto &fi = j["fileIndex"];
            if (fi.contains("engine"))
                m_fileIndexEngine = normalizedEngine(QString::fromStdString(fi["engine"]));
        }

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
    j["fileIndex"] = {
        {"engine", m_fileIndexEngine.toStdString()}
    };

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
QString ConfigManager::fileIndexEngine() const { return m_fileIndexEngine; }

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

void ConfigManager::setFileIndexEngine(const QString &engine)
{
    const QString normalized = normalizedEngine(engine);
    if (m_fileIndexEngine == normalized)
        return;

    m_fileIndexEngine = normalized;
    save();
    emit fileIndexEngineChanged();
}
