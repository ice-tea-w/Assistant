#pragma once

#include "core/plugin/PluginManifest.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QQmlEngine;

/// Creates and destroys plugin QML objects.
///
/// **This is the only module allowed to create or destroy plugin objects**, and the
/// only place that owns their QQmlContext and PluginHost (插件市场-模块框架.md §4.4).
///
/// Two invariants matter most:
///
///   1. A failed load must return the loader to "not loaded" with nothing leaked.
///      Partial state here turns into "the plugin half-works", which is much harder
///      to diagnose than a clean failure.
///   2. A failed load must **never** take the host down. Plugin QML is untrusted
///      student code; a syntax error is an expected condition, not a crash.
///
/// Thread: main thread only (QQmlEngine is not thread-safe).
class PluginLoader : public QObject
{
    Q_OBJECT

public:
    explicit PluginLoader(QObject *parent = nullptr);
    ~PluginLoader() override;

    /// The engine used to instantiate plugin QML. Must outlive every loaded plugin.
    void setEngine(QQmlEngine *engine);
    QQmlEngine *engine() const { return m_engine; }

    /// Instantiate `<pluginDir>/<manifest.main>` with a private context.
    ///
    /// @param error receives a ready-to-show reason on failure (goes into
    ///        InstalledPlugin::problem.detail)
    /// @return false when the plugin is already loaded, no engine is set, the file
    ///         is missing, or QML failed to compile/instantiate
    bool load(const PluginManifest &manifest, const QString &pluginDir,
              QString *error = nullptr);

    /// Destroy one plugin. Safe to call for an id that is not loaded.
    void unload(const QString &id);

    /// Destroy every loaded plugin (shutdown path).
    void unloadAll();

    QStringList loadedIds() const;                 ///< sorted
    bool isLoaded(const QString &id) const;
    int  count() const { return static_cast<int>(m_loaded.size()); }

    /// The instantiated QML root object of a loaded plugin, or nullptr.
    /// For host-side introspection and tests only — this is deliberately NOT
    /// reachable from plugin QML, and must not be handed to other plugins.
    QObject *pluginObject(const QString &id) const;

    /// Pages contributed by loaded plugins, ready for the navigation list.
    /// Each entry: { pluginId, pageId, title, icon, key ("<pluginId>:<pageId>"),
    ///               source (absolute file URL) }
    QVariantList navigationEntries() const;

    /// Instantiate one contributed page as a QQuickItem and return it.
    ///
    /// Crucially this uses the **plugin's own QQmlContext**, so `host` resolves
    /// inside the page exactly as it does in main.qml. Loading the page through a
    /// plain QML `Loader` instead would run it in the panel engine's root context
    /// where `host` does not exist.
    ///
    /// The item is cached per (plugin, page) so switching navigation back and forth
    /// keeps the page's state. Ownership stays here; callers must not delete it.
    /// @return nullptr on failure (missing page id, bad QML, plugin not loaded)
    QObject *createPage(const QString &pluginId, const QString &pageId,
                        QString *error = nullptr);

    /// Destroy every cached page of one plugin. Called from unload().
    void destroyPages(const QString &pluginId);

    /// Push host-wide facts into every live PluginHost. Called by PluginManager;
    /// plugins cannot set these themselves.
    void setAppVersion(const QString &appVersion);
    void setDark(bool dark);

signals:
    void loaded(const QString &id);
    void unloaded(const QString &id);

    /// Emitted after any load/unload so the navigation list can be recomputed.
    /// Kept separate from loaded()/unloaded() because those carry an id and are
    /// consumed by different listeners.
    void navigationChanged();

    /// A plugin called host.toast(); the manager decides how to surface it.
    void toastRequested(const QString &pluginId, const QString &text);

private:
    struct LoadedPlugin
    {
        PluginManifest manifest;
        QString        dir;
        QObject       *object    = nullptr;
        void          *component = nullptr;   ///< QQmlComponent*, void* to keep the header light
        void          *context   = nullptr;   ///< QQmlContext*
        QObject       *host      = nullptr;   ///< PluginHost*

        /// pageId -> instantiated QQuickItem, and its QQmlComponent for cleanup.
        /// The item is held through QPointer because QML reparents it into the page
        /// slot, and destroying that slot would delete it as a child — in which case
        /// our cleanup must not delete it a second time.
        QHash<QString, QPointer<QObject>> pageItems;
        QHash<QString, void *>            pageComponents;
    };

    void destroy(LoadedPlugin &plugin);
    void wireHost(LoadedPlugin &plugin);

    QQmlEngine *m_engine = nullptr;
    QHash<QString, LoadedPlugin> m_loaded;
};
