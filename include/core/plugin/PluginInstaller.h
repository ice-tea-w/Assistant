#pragma once

#include "core/plugin/PluginManifest.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class PluginRegistry;

/// Runs one install at a time and is **the only module allowed to write
/// `plugins/<id>/`** (插件市场-模块框架.md §4.3).
///
/// The two invariants that shape every line of this class:
///
///   1. **Before the commit point, a failure leaves nothing behind** — the temp
///      directory and the partial download are removed and `plugins.json` is not
///      touched. The user can simply retry.
///   2. **After the commit point, a failure does not roll the files back.** Once the
///      package sits in `plugins/<id>/`, "installed but broken" is a *different*
///      outcome from "install failed" and must be reported as such.
///
/// The commit point is the rename of the extracted package into `plugins/<id>/`.
///
/// Two entry points share one state machine:
///   * installFromZip()    — side-load, starts at Inspecting → Extracting
///   * installFromMarket() — starts at Inspecting → Downloading → Extracting
class PluginInstaller : public QObject
{
    Q_OBJECT

public:
    enum class Stage {
        Idle,
        Inspecting,    ///< does the source exist / is it usable
        Downloading,   ///< market installs only: fetch + hash
        Extracting,    ///< unzip into the temp directory (zip-slip guarded)
        Validating,    ///< find package root, parse and check plugin.json
        Committing,    ///< rename into plugins/<id>/, then record in plugins.json
        Done,
        Failed
    };
    Q_ENUM(Stage)

    explicit PluginInstaller(PluginRegistry *registry, QObject *parent = nullptr);

    /// Side-load a local .zip.
    /// @return requestId, or empty when rejected outright (busy / empty path)
    QString installFromZip(const QString &zipPath);

    /// Install from the market index.
    ///
    /// @param expectedVersion version declared by the index; the package's own
    ///        manifest must match it, otherwise the install is refused as
    ///        `version-mismatch` (the index and the bytes disagree — that is a
    ///        supply-chain smell, not something to paper over).
    /// @param sha256 expected lowercase hex digest; empty disables the check
    /// @param expectedSize used only for progress; 0 means unknown
    /// @return requestId, or empty when rejected outright
    QString installFromMarket(const QString &id, const QString &version,
                              const QUrl &url, const QString &sha256,
                              qint64 expectedSize);

    /// Ask to stop. Effective before the commit point (including mid-download,
    /// where it aborts the reply).
    /// @return false when the request is unknown or already past committing
    bool cancel(const QString &requestId);

    bool isBusy() const { return !m_activeId.isEmpty(); }
    QString activeRequestId() const { return m_activeId; }

signals:
    void progress(const QString &requestId, PluginInstaller::Stage stage,
                  int percent, const QString &detail);
    void log(const QString &requestId, const QString &line);

    /// Terminal signal. Exactly one per requestId, always emitted.
    void finished(const QString &requestId, bool ok, const QString &code,
                  const QString &message);

    /// Emitted just before finished(ok=true): what landed on disk, so the manager
    /// can enable it without re-reading the registry.
    void installed(const QString &pluginId, const QString &version);

private:
    struct Job
    {
        QString        requestId;
        QString        zipPath;        ///< local source (side-load) or download target
        QString        tempDir;
        QString        packageRoot;
        PluginManifest manifest;

        // ---- market install ----
        QUrl    url;
        QString expectedSha256;
        QString expectedVersion;
        qint64  expectedSize = 0;
        qint64  received     = 0;
        QPointer<QNetworkReply> reply;

        Stage  stage     = Stage::Idle;
        bool   cancelled = false;
        bool   committed = false;

        QString replacedVersion;       ///< existing install being replaced
    };

    void advance();
    void runInspect();
    void runDownload();        ///< starts the reply; completion is signalled
    void onDownloadReadyRead();
    void onDownloadFinished();
    void runExtract();
    void runValidate();
    void runCommit();

    void fail(const QString &code, const QString &message);
    void succeed();
    void emitLog(const QString &line);
    void writeLogFile(const QString &line);
    void cleanupTemp();

    /// Streaming SHA-256 of a file. Returns an empty string on I/O failure.
    static QString hashFile(const QString &path, QString *error);

    PluginRegistry         *m_registry = nullptr;
    QNetworkAccessManager  *m_nam = nullptr;

    QHash<QString, Job> m_jobs;
    QString             m_activeId;
};
