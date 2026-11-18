#pragma once

#include <QHash>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include "contract/IHostApi.h"
#include "core/plugin/PluginManifest.h"

class DisposalGroup;
class PluginExtensionRegistry;
class PluginServiceRegistry;
class QJSValue;

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

    /// 宿主实现的插件 API 版本（`contract::kApiVersion`）。
    ///
    /// 插件用它判断"宿主支持我要的 API 吗"；清单里对应的声明是 `engines.api`。
    /// 与 `appVersion`（宿主产品版本）不是一回事，二者都要看：
    /// API 版本管能力面，产品版本管行为/界面。
    Q_PROPERTY(int apiVersion READ apiVersion CONSTANT)

    /// 本插件**可用**的服务名（已按清单权限过滤）。`service(name)` 只会返回这里有的名字。
    Q_PROPERTY(QStringList services READ services CONSTANT)

    /// 最近一次 addExtension/service() 失败的原因；成功后清空。
    /// 这些调用返回 nullptr 而不是抛错，插件据此提示用户。
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

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
    int  apiVersion() const { return contract::kApiVersion; }
    QStringList services() const { return m_availableServices; }
    QString lastError() const { return m_lastError; }

    /// Set by PluginLoader from the running application; not plugin-controlled.
    void setAppVersion(const QString &appVersion);
    void setDark(bool dark);

    // ---- injected by PluginLoader (host side only) ----
    void setExtensionRegistry(PluginExtensionRegistry *registry);
    void setServiceRegistry(PluginServiceRegistry *registry);
    void setDisposalGroup(DisposalGroup *group);
    /// Names the plugin may use, already filtered by its manifest permissions.
    void setAvailableServices(const QStringList &names);
    /// The manifest of this plugin; service availability is derived from its
    /// permissions, so the host needs it rather than re-parsing `plugin.json`.
    void setManifest(const PluginManifest &manifest);

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

    // ---- 扩展点（插件 API 2 起）----

    /// Declared extension points: [{id, title, consumed}].
    Q_INVOKABLE QVariantList extensionPoints() const;

    /// Contribute to a point. Returns a handle whose `dispose()` withdraws it;
    /// the handle is tracked by this plugin's disposal group, so unload also removes
    /// it. Returns nullptr (and sets lastError) when the point is unknown or the
    /// payload misses a required key.
    Q_INVOKABLE QObject *addExtension(const QString &pointId, const QVariantMap &data);

    /// Withdraw everything this plugin contributed to @p pointId.
    /// @return how many were removed
    Q_INVOKABLE int clearExtensions(const QString &pointId);

    // ---- 服务（插件 API 2 起）----

    /// A per-plugin view of a host service, or nullptr when unavailable.
    /// Views are cached: repeated calls return the same object.
    Q_INVOKABLE QObject *service(const QString &name);

    // ---- 生命周期 ----

    /// Register cleanup that runs on unload/disable, after every extension handle is
    /// disposed (LIFO). Use it for timers/handlers the host cannot see.
    Q_INVOKABLE QObject *onDispose(QJSValue callback);

signals:
    void themeChanged();
    void lastErrorChanged();

    /// Relay point: PluginManager forwards this to the UI.
    void toastRequested(const QString &text);

private:
    bool loadSettings();
    bool saveSettings() const;

    void setLastError(const QString &message);

    QString m_pluginId;
    QString m_pluginName;
    QString m_version;
    QString m_dataDir;
    QString m_appVersion;
    bool    m_isDark = false;

    PluginExtensionRegistry *m_extensions = nullptr;
    PluginServiceRegistry   *m_services   = nullptr;
    DisposalGroup           *m_disposal   = nullptr;
    PluginManifest           m_manifest;
    QStringList              m_availableServices;
    QHash<QString, QPointer<QObject>> m_serviceViews;
    QString                  m_lastError;

    QVariantMap m_settings;
    bool m_settingsLoaded = false;
};
