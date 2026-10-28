#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

/// Local installed-state record for one plugin.
///
/// This mirrors what gets written into `plugins.json`. It intentionally holds **no
/// runtime object** — the instantiated QML object lives in PluginLoader and is not
/// persisted, which is what makes `plugins.json` the only unrecoverable piece of
/// state in the whole subsystem (see 插件市场-模块框架.md §5).
struct InstalledPlugin
{
    QString   id;
    QString   version;
    QDateTime installedAt;
    bool      enabled = true;

    struct Origin
    {
        QString kind;       ///< "market" | "local" | "bundled"
        QString url;        ///< market downloads only
        QString sha256;     ///< market downloads only
        QString path;       ///< side-loaded packages only
    } origin;

    /// Non-empty when the plugin is installed but unusable (bad manifest / QML
    /// failed to load). Per the LLD, this is NOT the same as "install failed":
    /// the files are intact, only the code is broken, so the user can uninstall.
    struct Problem
    {
        QString code;
        QString detail;
        bool isNull() const { return code.isEmpty(); }
    } problem;
};

/// Owns `<plugin root>/plugins.json` and the `plugins/` directory layout.
///
/// **This is the only module allowed to write `plugins.json`**, and the only
/// module whose state cannot be rebuilt from elsewhere. Everything else (catalog,
/// loader, installer) can be thrown away and recreated; if this file is lost the
/// user's "what did I install" knowledge is gone. That is why every write here
/// goes through QSaveFile (atomic replace) instead of in-place truncation.
///
/// Write-failure contract: a failed save never leaves memory diverged from disk.
/// Either both changed or neither did — so a caller may retry safely.
///
/// Thread: main thread only.
class PluginRegistry
{
public:
    /// @param rootDir the `plugins/` directory. Empty means derive it from
    ///        QStandardPaths (`<AppLocalData>/plugins`). Tests pass a temp dir.
    explicit PluginRegistry(const QString &rootDir = QString());

    // ---- persistence ----

    /// Read state from disk, always leaving the registry usable.
    ///
    /// Recovery behaviour (deliberate, not silent):
    ///   * file missing            -> empty registry, root created
    ///   * file corrupt            -> renamed to `plugins.json.broken-<ts>`,
    ///                                registry emptied and rebuilt
    ///   * unknown schemaVersion   -> treated as corrupt
    ///
    /// @param warning optional; receives a message when something was recovered.
    ///        Never used for "file missing" — that is a normal first run.
    /// @return false only when the root directory cannot be created at all,
    ///         i.e. the environment is broken beyond recovery.
    bool load(QString *warning = nullptr);

    /// Write the full state to `plugins.json` atomically.
    /// @return false on any I/O failure; memory is untouched either way.
    bool save(QString *error = nullptr) const;

    // ---- queries ----

    const QHash<QString, InstalledPlugin> &all() const { return m_plugins; }
    std::optional<InstalledPlugin> get(const QString &id) const;
    QStringList ids() const;                 ///< sorted, for stable UI ordering
    int  count() const { return m_plugins.size(); }
    bool contains(const QString &id) const { return m_plugins.contains(id); }

    // ---- mutations (memory only) ----

    /// Insert or replace. Does not touch disk; call save() or use upsertAndSave().
    bool upsert(const InstalledPlugin &plugin);

    /// Remove by id. @return false when the id was not present.
    bool remove(const QString &id);

    void clear() { m_plugins.clear(); }

    // ---- mutations (memory + disk, transactional) ----

    /// upsert() + save(), reverting memory if the save fails so that a caller can
    /// treat a false return as "nothing happened".
    bool upsertAndSave(const InstalledPlugin &plugin, QString *error = nullptr);

    /// remove() + save(), same rollback contract.
    bool removeAndSave(const QString &id, QString *error = nullptr);

    // ---- paths ----

    QString rootDir() const { return m_root; }
    QString stateFile() const;                       ///< <root>/plugins.json
    QString pluginDir(const QString &id) const;      ///< <root>/<id>
    QString dataDir(const QString &id) const;        ///< <root>/<id>/data
    QString cacheDir() const;                        ///< <root>/.cache
    QString tmpDir() const;                          ///< <root>/.tmp
    QString logsDir() const;                         ///< <root>/.logs
    QString trashDir() const;                        ///< <root>/.trash
    QString marketCacheFile() const;                 ///< <root>/market-cache.json

    /// Create the root directory tree if missing.
    bool ensureRoot(QString *error = nullptr) const;

    /// Create `<root>/<id>/data`, returning its path in `*path`.
    bool ensureDataDir(const QString &id, QString *path = nullptr,
                       QString *error = nullptr) const;

private:
    QHash<QString, InstalledPlugin> m_plugins;
    QString m_root;
};
