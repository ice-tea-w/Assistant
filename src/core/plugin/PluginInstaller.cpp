#include "core/plugin/PluginInstaller.h"

#include "core/Logger.h"
#include "core/plugin/PluginArchive.h"
#include "core/plugin/PluginRegistry.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

namespace {

/// Per-stage percentage bands so the UI bar moves monotonically.
int percentFor(PluginInstaller::Stage stage)
{
    switch (stage) {
    case PluginInstaller::Stage::Idle:        return 0;
    case PluginInstaller::Stage::Inspecting:  return 10;
    case PluginInstaller::Stage::Extracting:  return 35;
    case PluginInstaller::Stage::Validating:  return 65;
    case PluginInstaller::Stage::Committing:  return 85;
    case PluginInstaller::Stage::Done:        return 100;
    case PluginInstaller::Stage::Failed:      return 100;
    }
    return 0;
}

QString stageText(PluginInstaller::Stage stage)
{
    switch (stage) {
    case PluginInstaller::Stage::Idle:       return QStringLiteral("准备中");
    case PluginInstaller::Stage::Inspecting: return QStringLiteral("检查安装包");
    case PluginInstaller::Stage::Extracting: return QStringLiteral("解压中");
    case PluginInstaller::Stage::Validating: return QStringLiteral("校验清单");
    case PluginInstaller::Stage::Committing: return QStringLiteral("写入插件目录");
    case PluginInstaller::Stage::Done:       return QStringLiteral("完成");
    case PluginInstaller::Stage::Failed:     return QStringLiteral("失败");
    }
    return QString();
}

QString stamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

} // namespace

PluginInstaller::PluginInstaller(PluginRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
{
}

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------

QString PluginInstaller::installFromZip(const QString &zipPath)
{
    if (!m_registry) {
        Logger::get().error("PluginInstaller: no registry");
        return {};
    }

    if (isBusy()) {
        // Single-install serialisation (LLD §12.3): refusing is better than queueing,
        // because the UI has no way to express "your turn is next".
        Logger::get().warn("PluginInstaller: rejected — another install is running ({})",
                           m_activeId.toStdString());
        return {};
    }

    if (zipPath.trimmed().isEmpty())
        return {};

    Job job;
    job.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.zipPath   = zipPath;
    job.stage     = Stage::Inspecting;

    m_jobs.insert(job.requestId, job);
    m_activeId = job.requestId;

    Logger::get().info("PluginInstaller: start {} from {}", job.requestId.toStdString(),
                       zipPath.toStdString());

    emitLog(QStringLiteral("开始安装：%1").arg(zipPath));

    // Stages run through the event loop so a Cancel click can be handled between
    // them instead of waiting for the whole install to finish.
    QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);

    return job.requestId;
}

bool PluginInstaller::cancel(const QString &requestId)
{
    auto it = m_jobs.find(requestId);
    if (it == m_jobs.end())
        return false;

    if (it->committed)
        return false;              // "too-late": files are already in place

    it->cancelled = true;
    emitLog(QStringLiteral("已请求取消"));
    return true;
}

// ---------------------------------------------------------------------------
// stage driver
// ---------------------------------------------------------------------------

void PluginInstaller::advance()
{
    if (m_activeId.isEmpty())
        return;

    const auto it = m_jobs.constFind(m_activeId);
    if (it == m_jobs.constEnd())
        return;

    const QString requestId = it->requestId;
    const Stage   stage     = it->stage;
    const bool    cancelled = it->cancelled;
    const bool    committed = it->committed;

    if (cancelled && !committed) {
        emitLog(QStringLiteral("已取消"));
        cleanupTemp();
        emit finished(requestId, false, QStringLiteral("cancelled"),
                      QStringLiteral("已取消安装"));
        m_jobs.remove(requestId);
        m_activeId.clear();
        return;
    }

    emit progress(requestId, stage, percentFor(stage), stageText(stage));

    switch (stage) {
    case Stage::Inspecting: runInspect();  break;
    case Stage::Extracting: runExtract();  break;
    case Stage::Validating: runValidate(); break;
    case Stage::Committing: runCommit();   break;
    default:                break;
    }
}

// ---------------------------------------------------------------------------
// stage: Inspecting
// ---------------------------------------------------------------------------

void PluginInstaller::runInspect()
{
    // Work on a copy: fail() removes the job from m_jobs, so a reference into the
    // hash would dangle the moment we report an error.
    Job job = m_jobs.value(m_activeId);

    const QFileInfo info(job.zipPath);

    if (!info.exists()) {
        fail(QStringLiteral("not-found"), QStringLiteral("文件不存在：%1").arg(job.zipPath));
        return;
    }
    if (!info.isFile()) {
        fail(QStringLiteral("invalid-spec"), QStringLiteral("不是文件：%1").arg(job.zipPath));
        return;
    }

    job.tempDir = m_registry->tmpDir() + QLatin1Char('/') + job.requestId;
    emitLog(QStringLiteral("安装包大小 %1 字节").arg(info.size()));

    job.stage = Stage::Extracting;

    auto it = m_jobs.find(m_activeId);
    if (it == m_jobs.end())
        return;
    it->tempDir = job.tempDir;
    it->stage   = job.stage;

    QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// stage: Extracting
// ---------------------------------------------------------------------------

void PluginInstaller::runExtract()
{
    const Job job = m_jobs.value(m_activeId);

    const auto result = PluginArchive::extract(job.zipPath, job.tempDir);

    if (!result.ok) {
        fail(result.code, result.detail);
        return;
    }

    emitLog(QStringLiteral("解压完成，%1 个文件").arg(result.fileCount));

    auto it = m_jobs.find(m_activeId);
    if (it == m_jobs.end())
        return;
    it->packageRoot = result.packageRoot;
    it->stage       = Stage::Validating;

    QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// stage: Validating
// ---------------------------------------------------------------------------

void PluginInstaller::runValidate()
{
    const Job job = m_jobs.value(m_activeId);

    const QString manifestPath = job.packageRoot + QStringLiteral("/plugin.json");

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("not-a-plugin"),
             QStringLiteral("无法读取 plugin.json：%1").arg(manifestPath));
        return;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QString parseError;
    const auto parsed = PluginManifest::parse(raw, &parseError);
    if (!parsed) {
        fail(QStringLiteral("invalid-manifest"), parseError);
        return;
    }

    if (parsed->isNativeRuntime()) {
        fail(QStringLiteral("invalid-manifest"),
             QStringLiteral("本版本不支持原生插件（runtime=native）"));
        return;
    }

    // Environment gate: refuse a plugin built for an incompatible host *before*
    // anything is written, not after it fails to load.
    QString engineError;
    if (!parsed->checkEngine(QCoreApplication::applicationVersion(), &engineError)) {
        fail(QStringLiteral("incompatible-version"), engineError);
        return;
    }

    QString replaced;
    if (m_registry->contains(parsed->id)) {
        const auto existing = m_registry->get(parsed->id);
        if (existing && existing->version == parsed->version) {
            fail(QStringLiteral("already-installed"),
                 QStringLiteral("已安装同版本 %1").arg(parsed->version));
            return;
        }
        if (existing)
            replaced = existing->version;
    }

    emitLog(QStringLiteral("清单有效：%1 v%2").arg(parsed->id, parsed->version));
    if (!replaced.isEmpty())
        emitLog(QStringLiteral("将替换已安装的 v%1").arg(replaced));

    auto it = m_jobs.find(m_activeId);
    if (it == m_jobs.end())
        return;
    it->manifest        = *parsed;
    it->replacedVersion = replaced;
    it->stage           = Stage::Committing;

    QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// stage: Committing  —— 提交点
// ---------------------------------------------------------------------------

void PluginInstaller::runCommit()
{
    Job job = m_jobs.value(m_activeId);

    const QString target = m_registry->pluginDir(job.manifest.id);

    // Move an existing install aside first. This is still *before* the commit point,
    // so a failure here can be undone cleanly.
    QString backup;
    if (QDir(target).exists()) {
        QDir().mkpath(m_registry->trashDir());
        backup = m_registry->trashDir() + QLatin1Char('/') + job.manifest.id
               + QStringLiteral("-") + stamp();

        if (!QDir().rename(target, backup)) {
            fail(QStringLiteral("permission"),
                 QStringLiteral("无法移除旧版本目录：%1").arg(target));
            return;
        }
        emitLog(QStringLiteral("旧版本已移至 .trash/%1").arg(QFileInfo(backup).fileName()));
    }

    // ---- commit point: the package becomes visible at plugins/<id>/ ----
    if (!QDir().rename(job.packageRoot, target)) {
        // Restore the previous version: a failed upgrade must not destroy a working
        // install.
        if (!backup.isEmpty())
            QDir().rename(backup, target);

        fail(QStringLiteral("permission"),
             QStringLiteral("无法写入插件目录：%1").arg(target));
        return;
    }

    emitLog(QStringLiteral("已写入 %1").arg(target));

    // Mark committed before touching the registry: from here on, failures keep the
    // files (LLD §4.3 invariant 2).
    {
        auto it = m_jobs.find(m_activeId);
        if (it == m_jobs.end())
            return;
        it->committed = true;
    }

    InstalledPlugin entry;
    entry.id          = job.manifest.id;
    entry.version     = job.manifest.version;
    entry.installedAt = QDateTime::currentDateTimeUtc();
    entry.enabled     = false;                    // PluginManager decides to enable
    entry.origin.kind = QStringLiteral("local");
    entry.origin.path = job.zipPath;

    QString saveError;
    if (!m_registry->upsertAndSave(entry, &saveError)) {
        emitLog(QStringLiteral("状态写入失败：%1").arg(saveError));

        // Files are good, only the record failed. Succeeding with a warning beats
        // reporting "install failed" and sending the user into a reinstall loop.
        const QString requestId = job.requestId;
        emit finished(requestId, true, QString(),
                      QStringLiteral("插件已写入，但状态记录失败：%1").arg(saveError));
        cleanupTemp();
        m_jobs.remove(requestId);
        m_activeId.clear();
        return;
    }

    // The replaced version is no longer needed.
    if (!backup.isEmpty())
        QDir(backup).removeRecursively();

    succeed();
}

// ---------------------------------------------------------------------------
// terminal helpers
// ---------------------------------------------------------------------------

void PluginInstaller::fail(const QString &code, const QString &message)
{
    const auto it = m_jobs.constFind(m_activeId);
    if (it == m_jobs.constEnd())
        return;

    const QString requestId = it->requestId;
    const Stage   stage     = it->stage;
    const bool    committed = it->committed;

    emitLog(QStringLiteral("失败：%1 — %2").arg(code, message));
    Logger::get().warn("PluginInstaller: {} failed at stage {} — {}",
                       requestId.toStdString(), static_cast<int>(stage),
                       message.toStdString());

    emit progress(requestId, Stage::Failed, 100, message);

    // Pre-commit failure must leave nothing behind.
    if (!committed)
        cleanupTemp();

    emit finished(requestId, false, code, message);

    m_jobs.remove(requestId);
    m_activeId.clear();
}

void PluginInstaller::succeed()
{
    const auto it = m_jobs.constFind(m_activeId);
    if (it == m_jobs.constEnd())
        return;

    const QString requestId = it->requestId;
    const QString pluginId  = it->manifest.id;
    const QString version   = it->manifest.version;

    emitLog(QStringLiteral("安装完成：%1 v%2").arg(pluginId, version));
    Logger::get().info("PluginInstaller: installed {} v{}",
                       pluginId.toStdString(), version.toStdString());

    emit progress(requestId, Stage::Done, 100, stageText(Stage::Done));

    cleanupTemp();

    emit installed(pluginId, version);
    emit finished(requestId, true, QString(), QString());

    m_jobs.remove(requestId);
    m_activeId.clear();
}

void PluginInstaller::cleanupTemp()
{
    const auto it = m_jobs.constFind(m_activeId);
    if (it == m_jobs.constEnd())
        return;
    if (it->tempDir.isEmpty())
        return;

    QDir(it->tempDir).removeRecursively();
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

void PluginInstaller::emitLog(const QString &line)
{
    const QString requestId = m_activeId;
    if (requestId.isEmpty())
        return;

    writeLogFile(line);
    emit log(requestId, line);
}

void PluginInstaller::writeLogFile(const QString &line)
{
    if (!m_registry)
        return;

    QDir().mkpath(m_registry->logsDir());

    QFile file(m_registry->logsDir() + QLatin1Char('/') + m_activeId
               + QStringLiteral(".log"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;

    const QString stamped =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))
        + QStringLiteral("  ") + line + QLatin1Char('\n');

    file.write(stamped.toUtf8());
    file.close();
}
