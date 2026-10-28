#pragma once

#include "core/plugin/PluginManifest.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QQmlEngine;
class PluginRegistry;
class PluginLoader;

/// The single business entry point: **the only place that decides ordering**.
///
/// See 插件市场-模块框架.md §2.1 rule A. The four capability modules are deliberately
/// unaware of each other:
///
///   * PluginLoader never installs or writes plugins.json;
///   * PluginRegistry never loads QML;
///   * (later) PluginInstaller never enables anything by itself.
///
/// "Install then enable", "disable then maybe unload", "uninstall must unload first"
/// all live here. That keeps each capability module independently testable and lets
/// the UI express "install without enabling" later.
///
/// M0 scope: local plugins only. PluginCatalog (M1) and PluginInstaller (M2) are
/// optional collaborators — when they are null the market half simply reports empty
/// instead of pretending to work.
class PluginManager : public QObject
{
    Q_OBJECT

public:
    PluginManager(PluginRegistry *registry, PluginLoader *loader,
                  QObject *parent = nullptr);

    /// Inject the engine used by PluginLoader. Must be called before loading.
    void setEngine(QQmlEngine *engine);

    // ---- read side (what the UI consumes) ----

    /// Installed plugins as rows for PluginListModel / QVariantList binding.
    /// Each row: id, name, version, summary, icon, state, badge, badgeKind,
    ///           problem, percent, enabled, permissions
    QVariantList installedRows() const;

    /// Market entries that are not installed yet. Empty until M1 wires PluginCatalog.
    QVariantList availableRows() const;

    /// Pages contributed by loaded plugins (already in navigation-entry form).
    QVariantList navigationEntries() const;

    /// Instantiate (once, then cached) the page identified by "<pluginId>:<pageId>".
    /// Ownership stays in C++; QML only reparents and sizes the returned item.
    QObject *acquirePage(const QString &key);

    bool isOffline() const;
    QString fetchedAtText() const;

    // ---- lifecycle ----

    /// Register hand-dropped plugin directories, then load everything enabled.
    /// Failure for one plugin is recorded as its problem and never blocks the rest.
    void loadEnabledAtStartup();

    /// Unload every plugin (shutdown path).
    void unloadAll();

    /// Scan `<root>/<id>/plugin.json` for directories missing from plugins.json
    /// and register them with origin.kind = "local".
    /// @return how many were newly registered
    int discoverUnregistered();

    // ---- intents from the UI ----

    void setEnabled(const QString &id, bool enabled);
    void requestUninstall(const QString &id);
    void openPluginDir(const QString &id);

    /// Emit the current theme to every loaded plugin host.
    void setDark(bool dark);

signals:
    void viewChanged();          ///< any row changed (list must re-read)
    void navigationChanged();    ///< navigation items changed
    void toast(const QString &level, const QString &text);

private:
    /// Read and validate `<pluginDir>/plugin.json`, caching the result.
    bool loadManifestFor(const QString &id, PluginManifest *out, QString *error) const;

    /// Attach or clear a problem record and persist it.
    void setProblem(const QString &id, const QString &code, const QString &detail);

    /// Load one plugin through the loader, recording problems on failure.
    bool enablePlugin(const QString &id);

    QVariantList rowsFrom(const QStringList &ids) const;

    PluginRegistry *m_registry = nullptr;
    PluginLoader   *m_loader   = nullptr;

    mutable QHash<QString, PluginManifest> m_manifests;
};
