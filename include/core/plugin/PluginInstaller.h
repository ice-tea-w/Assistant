#pragma once

#include "core/plugin/PluginManifest.h"

#include <QHash>
#include <QObject>
#include <QString>

class PluginRegistry;

/// Runs one install at a time and is **the only module allowed to write
/// `plugins/<id>/`** (插件市场-模块框架.md §4.3).
///
/// The two invariants that shape every line of this class:
///
///   1. **Before the commit point, a failure leaves nothing behind** — the temp
///      directory is removed and `plugins.json` is not touched. The user can simply
///      retry; there is no "half installed" state to clean up by hand.
///   2. **After the commit point, a failure does not roll the files back.** Once the
///      package sits in `plugins/<id>/`, "installed but broken" is a *different*
///      outcome from "install failed" and must be reported as such, otherwise the
///      user gets stuck in a reinstall loop over files that are actually fine.
///
/// The commit point is the rename of the extracted package into `plugins/<id>/`.
///
/// v1 scope: local .zip only. URL downloads and market resolution plug in at the
/// `Inspecting`/`Extracting` stages without changing this state machine.
class PluginInstaller : public QObject
{
    Q_OBJECT

public:
    enum class Stage {
        Idle,
        Inspecting,    ///< does the source exist / is it usable
        Extracting,    ///< unzip into the temp directory (zip-slip guarded)
        Validating,    ///< find package root, parse and check plugin.json
        Committing,    ///< rename into plugins/<id>/, then record in plugins.json
        Done,
        Failed
    };
    Q_ENUM(Stage)

    explicit PluginInstaller(PluginRegistry *registry, QObject *parent = nullptr);

    /// Start installing from a local .zip (side-load).
    /// @return requestId, or an empty string when the request was rejected outright
    ///         (already busy, empty path)
    QString installFromZip(const QString &zipPath);

    /// Ask to stop. Only effective before the commit point.
    /// @return false when the request is unknown or already past committing
    ///         ("too-late" — the caller should tell the user the same way)
    bool cancel(const QString &requestId);

    bool isBusy() const { return !m_activeId.isEmpty(); }
    QString activeRequestId() const { return m_activeId; }

signals:
    void progress(const QString &requestId, PluginInstaller::Stage stage,
                  int percent, const QString &detail);
    void log(const QString &requestId, const QString &line);

    /// Terminal signal. Exactly one per requestId, always emitted.
    /// @param code empty on success, otherwise an LLD §7.3 code
    void finished(const QString &requestId, bool ok, const QString &code,
                  const QString &message);

    /// Emitted just before finished(ok=true): what landed on disk, so the manager
    /// can enable it without re-reading the registry. Purely additive to the
    /// LLD §7 signal set.
    void installed(const QString &pluginId, const QString &version);

private:
    struct Job
    {
        QString        requestId;
        QString        zipPath;
        QString        tempDir;
        QString        packageRoot;
        PluginManifest manifest;

        Stage  stage     = Stage::Idle;
        bool   cancelled = false;
        bool   committed = false;

        /// Existing install we are replacing (empty on a fresh install)
        QString replacedVersion;
    };

    void advance();                       ///< run the current stage, then queue the next
    void runInspect();
    void runExtract();
    void runValidate();
    void runCommit();

    void fail(const QString &code, const QString &message);
    void succeed();
    void emitLog(const QString &line);
    void writeLogFile(const QString &line);
    void cleanupTemp();

    PluginRegistry *m_registry = nullptr;

    QHash<QString, Job> m_jobs;
    QString             m_activeId;
};
