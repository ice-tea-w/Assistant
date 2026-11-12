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
    // A7 起 v2 是唯一实现（旧 FileSearcher 已从产品路径删除）。
    m_fileIndexHardLimit = 1000000;   // §3.3 硬保护上限（软预算 30 万只记日志）

    // Phase B 治理：默认“全都要 + 排除噪音 + 工程目录跳过”
    m_fileClassifier = FileClassifier();
    m_excludeRules   = ExcludeRuleSet();
    m_kindFilter     = KindFilter();
    m_kindOverrides.clear();

    // Phase C：收件箱默认关闭（绝不默认搬用户文件），路径留空 → AppConfigLocation/Inbox
    m_inboxEnabled  = false;
    m_inboxPath.clear();
    m_inboxSettleMs = 1200;
    m_inboxTrackingWindowSec = 60;
    m_inboxExtraWatchDirs.clear();
    m_inboxAutoOrganize = false;
    m_inboxRules.clear();

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

        // file index engine (A2 的 A/B 开关) + 硬保护上限 (A3, §3.3)
        if (j.contains("fileIndex")) {
            auto &fi = j["fileIndex"];
            // A7：fileIndex.engine 已删除（v2 是唯一实现）；旧配置里的这个键会被忽略，
            // 下次 save() 时自然消失。
            if (fi.contains("hardLimitEntries")) {
                const int v = fi["hardLimitEntries"].get<int>();
                if (v > 0)
                    m_fileIndexHardLimit = v;
            }

            // ---- Phase B：治理配置 ----
            if (fi.contains("kindFilter") && fi["kindFilter"].is_object()) {
                auto &kf = fi["kindFilter"];
                m_kindFilter = KindFilter();
                m_kindFilter.mode = KindFilter::modeFromString(
                    QString::fromStdString(kf.value("mode", std::string("all"))));

                if (kf.contains("kinds") && kf["kinds"].is_array()) {
                    for (const auto &item : kf["kinds"]) {
                        const uint16_t mask = FileKind::fromToken(
                            QString::fromStdString(item.get<std::string>()));
                        m_kindFilter.kinds = static_cast<uint16_t>(m_kindFilter.kinds | mask);
                    }
                }
                if (kf.contains("extraExtensions") && kf["extraExtensions"].is_array()) {
                    for (const auto &item : kf["extraExtensions"]) {
                        QString ext = QString::fromStdString(item.get<std::string>()).toLower();
                        while (ext.startsWith(QLatin1Char('.')))
                            ext.remove(0, 1);
                        if (!ext.isEmpty()) {
                            m_kindFilter.extraExtensions.insert(ext);
                            m_fileClassifier.addExtraExtension(ext);
                        }
                    }
                }
                if (kf.contains("kindOverrides") && kf["kindOverrides"].is_object()) {
                    m_kindOverrides.clear();
                    for (auto it = kf["kindOverrides"].begin(); it != kf["kindOverrides"].end(); ++it) {
                        if (!it.value().is_string())
                            continue;
                        const QString token = QString::fromStdString(
                            it.value().get<std::string>()).toLower();
                        const uint16_t mask = FileKind::fromToken(QStringView(token));
                        if (mask == 0)
                            continue;
                        const QString ext = QString::fromStdString(it.key()).toLower();
                        m_kindOverrides.insert(ext, token);
                        m_fileClassifier.setOverride(ext, mask);
                    }
                }
            }

            if (fi.contains("exclude") && fi["exclude"].is_object()) {
                auto &ex = fi["exclude"];
                m_excludeRules = ExcludeRuleSet();

                auto readList = [](const json &node) {
                    QStringList out;
                    if (!node.is_array())
                        return out;
                    for (const auto &item : node) {
                        if (item.is_string())
                            out.append(QString::fromStdString(item.get<std::string>()));
                    }
                    return out;
                };

                if (ex.contains("dirNames"))
                    m_excludeRules.setDirNames(readList(ex["dirNames"]));
                if (ex.contains("dirGlobs"))
                    m_excludeRules.setDirGlobs(readList(ex["dirGlobs"]));
                if (ex.contains("fileGlobs"))
                    m_excludeRules.setFileGlobs(readList(ex["fileGlobs"]));
                if (ex.contains("projectSentinels"))
                    m_excludeRules.setProjectSentinels(readList(ex["projectSentinels"]));

                const qint64 minSize = ex.value("minSizeBytes", 0LL);
                const qint64 maxMb   = ex.value("maxSizeMB", 0LL);
                m_excludeRules.setSizeLimits(minSize, maxMb > 0 ? maxMb * 1024 * 1024 : 0);

                if (ex.contains("projectPolicy")) {
                    m_excludeRules.setProjectPolicy(ExcludeRuleSet::projectPolicyFromString(
                        QString::fromStdString(ex.value("projectPolicy", std::string("skip")))));
                }
                if (ex.contains("excludeProjectBuildDirs")) {
                    m_excludeRules.setExcludeProjectBuildDirs(
                        ex.value("excludeProjectBuildDirs", true));
                }
            }
        }
        // 下载接管（Phase C）
        if (j.contains("inbox")) {
            auto &in = j["inbox"];
            if (in.contains("enabled"))   m_inboxEnabled  = in.value("enabled", false);
            if (in.contains("path"))      m_inboxPath     = QString::fromStdString(in.value("path", std::string()));
            if (in.contains("settleMs")) {
                const int ms = in.value("settleMs", 1200);
                if (ms >= 300 && ms <= 60000)
                    m_inboxSettleMs = ms;
            }
            if (in.contains("tracking") && in["tracking"].is_object()) {
                auto &tr = in["tracking"];
                const int sec = tr.value("windowSec", 60);
                if (sec >= 1 && sec <= 3600)
                    m_inboxTrackingWindowSec = sec;
                if (tr.contains("extraWatchDirs") && tr["extraWatchDirs"].is_array()) {
                    m_inboxExtraWatchDirs.clear();
                    for (const auto &item : tr["extraWatchDirs"]) {
                        if (item.is_string())
                            m_inboxExtraWatchDirs.append(
                                QString::fromStdString(item.get<std::string>()));
                    }
                }
            }
            if (in.contains("autoOrganize"))
                m_inboxAutoOrganize = in.value("autoOrganize", false);
            if (in.contains("rules") && in["rules"].is_array()) {
                m_inboxRules.clear();
                for (const auto &item : in["rules"]) {
                    if (!item.is_object())
                        continue;
                    QVariantMap rule;
                    rule.insert(QStringLiteral("name"),
                                QString::fromStdString(item.value("name", std::string())));
                    rule.insert(QStringLiteral("priority"), item.value("priority", 100));
                    rule.insert(QStringLiteral("enabled"), item.value("enabled", true));
                    rule.insert(QStringLiteral("action"),
                                QString::fromStdString(item.value("action", std::string("suggest"))));
                    rule.insert(QStringLiteral("dest"),
                                QString::fromStdString(item.value("dest", std::string())));
                    if (item.contains("match") && item["match"].is_object()) {
                        const auto &match = item["match"];
                        rule.insert(QStringLiteral("kind"),
                                    QString::fromStdString(match.value("kind", std::string())));
                        rule.insert(QStringLiteral("size"),
                                    QString::fromStdString(match.value("size", std::string())));
                    } else {
                        rule.insert(QStringLiteral("kind"),
                                    QString::fromStdString(item.value("kind", std::string())));
                        rule.insert(QStringLiteral("size"),
                                    QString::fromStdString(item.value("size", std::string())));
                    }
                    m_inboxRules.append(rule);
                }
            }
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
    j["fileIndex"] = {
        {"hardLimitEntries",  m_fileIndexHardLimit}
    };

    // Phase B：治理配置写回（kindFilter / exclude 都是可选节，写全便于用户手改）
    {
        auto &fi = j["fileIndex"];

        fi["kindFilter"]["mode"] = KindFilter::modeToString(m_kindFilter.mode).toStdString();
        fi["kindFilter"]["kinds"] = json::array();
        for (const QString &token : FileKind::allTokens()) {
            const uint16_t mask = FileKind::fromToken(QStringView(token));
            if (mask != 0 && (m_kindFilter.kinds & mask) != 0)
                fi["kindFilter"]["kinds"].push_back(token.toStdString());
        }
        fi["kindFilter"]["extraExtensions"] = json::array();
        for (const QString &ext : m_kindFilter.extraExtensions)
            fi["kindFilter"]["extraExtensions"].push_back(ext.toStdString());

        fi["kindFilter"]["kindOverrides"] = json::object();
        for (auto it = m_kindOverrides.constBegin(); it != m_kindOverrides.constEnd(); ++it) {
            fi["kindFilter"]["kindOverrides"][it.key().toStdString()] =
                it.value().toString().toStdString();
        }
        fi["exclude"]["dirNames"]  = json::array();
        for (const QString &s : m_excludeRules.dirNames())
            fi["exclude"]["dirNames"].push_back(s.toStdString());
        fi["exclude"]["dirGlobs"] = json::array();
        for (const QString &s : m_excludeRules.dirGlobs())
            fi["exclude"]["dirGlobs"].push_back(s.toStdString());
        fi["exclude"]["fileGlobs"] = json::array();
        for (const QString &s : m_excludeRules.fileGlobs())
            fi["exclude"]["fileGlobs"].push_back(s.toStdString());
        fi["exclude"]["projectSentinels"] = json::array();
        for (const QString &s : m_excludeRules.projectSentinels())
            fi["exclude"]["projectSentinels"].push_back(s.toStdString());
        fi["exclude"]["minSizeBytes"] = m_excludeRules.minSizeBytes();
        fi["exclude"]["maxSizeMB"] = m_excludeRules.maxSizeBytes() > 0
                                         ? m_excludeRules.maxSizeBytes() / (1024 * 1024)
                                         : 0;
        fi["exclude"]["projectPolicy"] =
            ExcludeRuleSet::projectPolicyToString(m_excludeRules.projectPolicy()).toStdString();
        fi["exclude"]["excludeProjectBuildDirs"] = m_excludeRules.excludesProjectBuildDirs();
    }
    j["inbox"] = {
        {"enabled",  m_inboxEnabled},
        {"path",     m_inboxPath.toStdString()},
        {"settleMs", m_inboxSettleMs},
        {"firstRunPolicy", "newOnly"}
    };
    j["inbox"]["tracking"] = {
        {"windowSec", m_inboxTrackingWindowSec},
        {"extraWatchDirs", json::array()},
        {"useUsnJournal", false},
        {"notifyOnFiledAway", false}
    };
    for (const QString &dir : m_inboxExtraWatchDirs)
        j["inbox"]["tracking"]["extraWatchDirs"].push_back(dir.toStdString());

    j["inbox"]["autoOrganize"] = m_inboxAutoOrganize;
    j["inbox"]["rules"] = json::array();
    for (const QVariant &v : m_inboxRules) {
        const QVariantMap rule = v.toMap();
        j["inbox"]["rules"].push_back({
            { "name", rule.value(QStringLiteral("name")).toString().toStdString() },
            { "priority", rule.value(QStringLiteral("priority"), 100).toInt() },
            { "enabled", rule.value(QStringLiteral("enabled"), true).toBool() },
            { "action", rule.value(QStringLiteral("action"), QStringLiteral("suggest")).toString().toStdString() },
            { "dest", rule.value(QStringLiteral("dest")).toString().toStdString() },
            { "match", {
                { "kind", rule.value(QStringLiteral("kind")).toString().toStdString() },
                { "size", rule.value(QStringLiteral("size")).toString().toStdString() }
            } }
        });
    }
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
int     ConfigManager::fileIndexHardLimitEntries() const { return m_fileIndexHardLimit; }
const FileClassifier &ConfigManager::fileClassifier() const { return m_fileClassifier; }
const ExcludeRuleSet &ConfigManager::excludeRules() const  { return m_excludeRules; }
const KindFilter     &ConfigManager::kindFilter() const    { return m_kindFilter; }

QString ConfigManager::fileKindFilterMode() const
{
    return KindFilter::modeToString(m_kindFilter.mode);
}

QStringList ConfigManager::fileKindFilterKinds() const
{
    QStringList out;
    for (const QString &token : FileKind::allTokens()) {
        const uint16_t mask = FileKind::fromToken(QStringView(token));
        if (mask != 0 && (m_kindFilter.kinds & mask) != 0)
            out.append(token);
    }
    return out;
}

QString ConfigManager::fileProjectPolicy() const
{
    return ExcludeRuleSet::projectPolicyToString(m_excludeRules.projectPolicy());
}

bool ConfigManager::inboxEnabled() const
{
    return m_inboxEnabled;
}

QString ConfigManager::inboxPath() const
{
    if (!m_inboxPath.isEmpty())
        return m_inboxPath;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = QDir::currentPath() + "/config";
    return dir + QStringLiteral("/Inbox");
}

int ConfigManager::inboxSettleMs() const
{
    return m_inboxSettleMs;
}

int ConfigManager::inboxTrackingWindowSec() const
{
    return m_inboxTrackingWindowSec;
}

QStringList ConfigManager::inboxExtraWatchDirs() const
{
    return m_inboxExtraWatchDirs;
}

bool ConfigManager::inboxAutoOrganize() const
{
    return m_inboxAutoOrganize;
}

QVariantList ConfigManager::inboxRules() const
{
    return m_inboxRules;
}

void ConfigManager::setInboxEnabled(bool enabled)
{
    if (m_inboxEnabled == enabled)
        return;
    m_inboxEnabled = enabled;
    save();
    Logger::get().info("ConfigManager: 下载接管 = {}", enabled ? "on" : "off");
    emit inboxChanged();
}

void ConfigManager::setInboxPath(const QString &path)
{
    if (m_inboxPath == path)
        return;
    m_inboxPath = path;
    save();
    emit inboxChanged();
}

void ConfigManager::setFileKindFilter(const QString &mode, const QStringList &kinds)
{
    KindFilter next = m_kindFilter;
    next.mode = KindFilter::modeFromString(mode);
    next.kinds = 0;
    for (const QString &token : kinds) {
        const uint16_t mask = FileKind::fromToken(QStringView(token));
        next.kinds = static_cast<uint16_t>(next.kinds | mask);
    }

    if (next == m_kindFilter)
        return;

    m_kindFilter = next;
    save();
    Logger::get().info("ConfigManager: 文件白名单 = {} / {} 类",
                       KindFilter::modeToString(m_kindFilter.mode).toStdString(),
                       kinds.join(QLatin1Char(',')).toStdString());
    emit fileKindFilterChanged();
}

void ConfigManager::setFileProjectPolicy(const QString &policy)
{
    const ExcludeRuleSet::ProjectPolicy next =
        ExcludeRuleSet::projectPolicyFromString(policy);
    if (next == m_excludeRules.projectPolicy())
        return;

    m_excludeRules.setProjectPolicy(next);
    save();
    Logger::get().info("ConfigManager: 工程目录策略 = {}",
                       ExcludeRuleSet::projectPolicyToString(next).toStdString());
    emit fileProjectPolicyChanged();
}

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

void ConfigManager::setFileIndexHardLimitEntries(int entries)
{
    if (entries <= 0 || m_fileIndexHardLimit == entries)
        return;

    m_fileIndexHardLimit = entries;
    save();
    emit fileIndexHardLimitChanged();
}

// ---- 设备同步 setters ----

void ConfigManager::setInboxAutoOrganize(bool enabled)
{
    if (m_inboxAutoOrganize == enabled)
        return;
    m_inboxAutoOrganize = enabled;
    save();
    Logger::get().info("ConfigManager: 自动归档 = {}", enabled ? "on" : "off");
    emit inboxChanged();
}

void ConfigManager::setInboxRules(const QVariantList &rules)
{
    m_inboxRules = rules;
    save();
    Logger::get().info("ConfigManager: 归档规则已更新（{} 条）", rules.size());
    emit inboxRulesChanged();
}

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
