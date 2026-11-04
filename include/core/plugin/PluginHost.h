#pragma once

#include <QObject>
#include <QString>
#include <QVariant>

/// The **only** surface a plugin is allowed to touch.
///
/// Design rule (插件市场-模块框架.md §4.5): the host gives "data + actions", never
/// "controls" and never internal objects. A plugin must not be able to reach
/// PluginRegistry, PluginManager or the QQmlEngine — if it could, turning plugin
/// execution into a cross-process protocol later would become impossible, because
/// plugin code would already depend on host internals.
///
/// One instance per loaded plugin, exposed to that plugin's QML context under the
/// name `host`. Owned by PluginLoader and destroyed on unload.
///
/// Everything a plugin writes goes into its own `data/` directory:
///   <plugin>/data/settings.json   — key/value store behind setting()/setSetting()
/// The plugin install directory itself is read-only by contract (it is replaced
/// wholesale on upgrade, so anything written there would be lost).
class PluginHost : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString pluginId   READ pluginId   CONSTANT)
    Q_PROPERTY(QString pluginName READ pluginName CONSTANT)
    Q_PROPERTY(QString version    READ version    CONSTANT)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString dataDir    READ dataDir    CONSTANT)
    Q_PROPERTY(bool    isDark     READ isDark     NOTIFY themeChanged)

public:
    /// @param dataDir the plugin's writable directory; created on first write
    PluginHost(QString pluginId, QString pluginName, QString version,
               QString dataDir, QObject *parent = nullptr);

    QString pluginId() const { return m_pluginId; }
    QString pluginName() const { return m_pluginName; }
    QString version() const { return m_version; }
    QString appVersion() const { return m_appVersion; }
    QString dataDir() const { return m_dataDir; }
    bool isDark() const { return m_isDark; }

    /// Set by PluginLoader from the running application; not plugin-controlled.
    void setAppVersion(const QString &appVersion);
    void setDark(bool dark);

    // ---- actions exposed to QML ----

    /// @param level "debug" | "info" | "warn" | "error"
    Q_INVOKABLE void log(const QString &level, const QString &message);

    /// Read a persisted key. Returns `fallback` when absent.
    Q_INVOKABLE QVariant setting(const QString &key, const QVariant &fallback = QVariant()) const;

    /// Persist a key. Writes the whole settings file atomically.
    /// @return false when the write failed (the in-memory value stays unchanged)
    Q_INVOKABLE bool setSetting(const QString &key, const QVariant &value);

    /// Ask the host to show a short message. The host decides how (toast today).
    Q_INVOKABLE void toast(const QString &text);

    /// Open a path or URL in the system handler.
    /// Restricted to the plugin's own data directory and https/mailto URLs —
    /// a plugin must not be able to launch arbitrary local executables.
    Q_INVOKABLE bool openPath(const QString &path);

signals:
    void themeChanged();

    /// Relay point: PluginManager forwards this to the UI.
    void toastRequested(const QString &text);

private:
    bool loadSettings();
    bool saveSettings() const;

    QString m_pluginId;
    QString m_pluginName;
    QString m_version;
    QString m_dataDir;
    QString m_appVersion;
    bool    m_isDark = false;

    QVariantMap m_settings;
    bool m_settingsLoaded = false;
};
