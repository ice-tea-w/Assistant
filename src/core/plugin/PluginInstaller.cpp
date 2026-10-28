#include "core/plugin/PluginInstaller.h"

#include "core/Logger.h"
#include "core/plugin/PluginArchive.h"
#include "core/plugin/PluginRegistry.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

namespace {

int percentFor(PluginInstaller::Stage stage)
{
    switch (stage) {
    case PluginInstaller::Stage::Idle:        return 0;
    case PluginInstaller::Stage::Inspecting:  return 5;
    case PluginInstaller::Stage::Downloading: return 20;
    case PluginInstaller::Stage::Extracting:  return 55;
    case PluginInstaller::Stage::Validating:  return 75;
    case PluginInstaller::Stage::Committing:  return 90;
    case PluginInstaller::Stage::Done:        return 100;
    case PluginInstaller::Stage::Failed:      return 100;
    }
    return 0;
}

QString stageText(PluginInstaller::Stage stage)
{
    switch (stage) {
    case PluginInstaller::Stage::Idle:        return QStringLiteral("准备中");
    case PluginInstaller::Stage::Inspecting:  return QStringLiteral("检查安装源");
    case PluginInstaller::Stage::Downloading: return QStringLiteral("下载中");
    case PluginInstaller::Stage::Extracting:  return QStringLiteral("解压中");
    case PluginInstaller::Stage::Validating:  return QStringLiteral("校验清单");
    case PluginInstaller::Stage::Committing:  return QStringLiteral("写入插件目录");
    case PluginInstaller::Stage::Done:        return QStringLiteral("完成");
    case PluginInstaller::Stage::Failed:      return QStringLiteral("失败");
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
// entry points
// ---------------------------------------------------------------------------

namespace {

QString newRequestId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

QString PluginInstaller::installFromZip(const QString &zipPath)
{
    if (!m_registry || isBusy() || zipPath.trimmed().isEmpty())
        return {};

    Job job;
    job.requestId = newRequestId();
    job.zipPath   = zipPath;
    job.stage     = Stage::Inspecting;

    m_jobs.insert(job.requestId, job);
    m_activeId = job.requestId;

    Logger::get().info("PluginInstaller: start {} (zip {})",
                       job.requestId.toStdString(), zipPath.toStdString());
    emitLog(QStringLiteral("开始安装：%1").arg(zipPath));

    QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);
    return job.requestId;
}

QString PluginInstaller::installFromMarket(const QString &id, const QString &version,
                                           const QUrl &url, const QString &sha256,
                                           qint64 expectedSize)
{
    if (!m_registry || isBusy())
        return {};

    if (!url.isValid() || url.isEmpty())
        return {};

    Job job;
    job.requestId       = newRequestId();
    job.url             = url;
    job.expectedSha256  = sha256.trimmed().toLower();
    job.expectedVersion = version;
    job.expectedSize    = expectedSize;
    job.stage           = Stage::Inspecting;

    m_jobs.insert(job.requestId, job);
    m_activeId = job.requestId;

    Logger::get().info("PluginInstaller: start {} (market {} v{})",
                       job.requestId.toStdString(), id.toStdString(),
                       version.toStdString());
    emitLog(QStringLiteral("开始安装：%1 v%2").arg(id, version));
    emitLog(QStringLiteral("下载地址：%1").arg(url.toString()));

    QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);
    return job.requestId;
}

bool PluginInstaller::cancel(const QString &requestId)
{
    auto it = m_jobs.find(requestId);
    if (it == m_jobs.end())
        return false;

    if (it->committed)
        return false;

    it->cancelled = true;

    // Abort an in-flight download immediately instead of waiting for it to finish.
    if (it->reply)
        it->reply->abort();

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
    case Stage::Downloading: runDownload(); break;   // async: finishes in a slot
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
    Job job = m_jobs.value(m_activeId);

    // ---- market install: nothing local to check, go straight to download ----
    if (!job.url.isEmpty()) {
        auto it = m_jobs.find(m_activeId);
        if (it == m_jobs.end())
            return;
        it->stage = Stage::Downloading;
        QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);
        return;
    }

    // ---- side-load: the zip must already exist ----
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

    auto it = m_jobs.find(m_activeId);
    if (it == m_jobs.end())
        return;
    it->tempDir = job.tempDir;
    it->stage   = Stage::Extracting;

    QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// stage: Downloading  (async)
// ---------------------------------------------------------------------------

void PluginInstaller::runDownload()
{
    Job job = m_jobs.value(m_activeId);

    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    // Download into the temp tree so every failure path cleans it up together.
    job.tempDir = m_registry->tmpDir() + QLatin1Char('/') + job.requestId;
    if (!QDir().mkpath(job.tempDir)) {
        fail(QStringLiteral("permission"),
             QStringLiteral("无法创建临时目录：%1").arg(job.tempDir));
        return;
    }

    job.zipPath = job.tempDir + QStringLiteral("/package.zip");

    auto *file = new QFile(job.zipPath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        fail(QStringLiteral("permission"),
             QStringLiteral("无法写入临时文件：%1").arg(job.zipPath));
        return;
    }

    QNetworkRequest request(job.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);

    QNetworkReply *reply = m_nam->get(request);

    {
        auto it = m_jobs.find(m_activeId);
        if (it == m_jobs.end()) {
            delete file;
            reply->abort();
            reply->deleteLater();
            return;
        }
        it->zipPath = job.zipPath;
        it->tempDir = job.tempDir;
        it->reply   = reply;
        it->received = 0;
    }

    // The QFile is owned by the reply lifetime; delete it when the reply dies.
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, file]() {
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;

        if (file->write(chunk) != chunk.size()) {
            reply->abort();
            return;
        }

        auto it = m_jobs.find(m_activeId);
        if (it == m_jobs.end() || it->reply != reply)
            return;

        it->received += chunk.size();

        const int percent = (it->expectedSize > 0)
            ? percentFor(Stage::Downloading)
              + static_cast<int>((percentFor(Stage::Extracting) - percentFor(Stage::Downloading))
                                 * (static_cast<double>(it->received) / it->expectedSize))
            : percentFor(Stage::Downloading);

        emit progress(it->requestId, Stage::Downloading,
                      qBound(percentFor(Stage::Downloading), percent,
                             percentFor(Stage::Extracting) - 1),
                      stageText(Stage::Downloading));
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, file]() {
        file->close();
        delete file;
        onDownloadFinished();
    });
}

void PluginInstaller::onDownloadReadyRead()
{
    // Handled inline by the lambda in runDownload(); kept for future refactors.
}

void PluginInstaller::onDownloadFinished()
{
    auto it = m_jobs.find(m_activeId);
    if (it == m_jobs.end())
        return;

    QNetworkReply *reply = it->reply;
    if (!reply)
        return;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto netError = reply->error();
    const QString netErrorText = reply->errorString();

    it->reply = nullptr;
    reply->deleteLater();

    // A cancel aborts the reply, which surfaces as OperationCanceledError.
    if (it->cancelled) {
        const QString requestId = it->requestId;
        emitLog(QStringLiteral("已取消"));
        cleanupTemp();
        emit finished(requestId, false, QStringLiteral("cancelled"),
                      QStringLiteral("已取消安装"));
        m_jobs.remove(requestId);
        m_activeId.clear();
        return;
    }

    // HTTP status is checked *before* the network error: Qt maps 4xx/5xx onto
    // QNetworkReply errors, so testing the error first would report a useless
    // "network" failure for a plain 404.
    if (status >= 400) {
        fail(QStringLiteral("http-status"), QStringLiteral("下载失败（HTTP %1）").arg(status));
        return;
    }

    if (netError != QNetworkReply::NoError) {
        fail(QStringLiteral("network"), QStringLiteral("下载失败：%1").arg(netErrorText));
        return;
    }

    if (status != 0 && (status < 200 || status >= 300)) {
        fail(QStringLiteral("http-status"), QStringLiteral("下载失败（HTTP %1）").arg(status));
        return;
    }

    const QFileInfo info(it->zipPath);
    if (!info.exists() || info.size() == 0) {
        fail(QStringLiteral("network"), QStringLiteral("下载内容为空"));
        return;
    }

    emitLog(QStringLiteral("下载完成，%1 字节").arg(info.size()));

    if (it->expectedSize > 0 && info.size() != it->expectedSize) {
        emitLog(QStringLiteral("注意：大小与索引声明不一致（索引 %1，实际 %2）")
                    .arg(it->expectedSize).arg(info.size()));
    }

    // ---- integrity check ----
    if (!it->expectedSha256.isEmpty()) {
        QString hashError;
        const QString actual = hashFile(it->zipPath, &hashError);

        if (actual.isEmpty()) {
            fail(QStringLiteral("checksum"),
                 QStringLiteral("无法计算校验值：%1").arg(hashError));
            return;
        }

        if (actual != it->expectedSha256) {
            emitLog(QStringLiteral("校验值不符：期望 %1，实际 %2")
                        .arg(it->expectedSha256, actual));
            fail(QStringLiteral("checksum"),
                 QStringLiteral("文件校验失败，可能被篡改或不完整"));
            return;
        }

        emitLog(QStringLiteral("校验通过（sha256 %1）").arg(actual.left(16) + QStringLiteral("…")));
    } else {
        emitLog(QStringLiteral("索引未提供校验值，跳过完整性校验（不建议）"));
    }

    it->stage = Stage::Extracting;
    QMetaObject::invokeMethod(this, &PluginInstaller::advance, Qt::QueuedConnection);
}

QString PluginInstaller::hashFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);

    // Stream it: a plugin package is small today, but there is no reason to load a
    // whole file into memory just to hash it.
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(64 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            if (error)
                *error = file.errorString();
            return {};
        }
        hash.addData(chunk);
    }

    file.close();
    return QString::fromLatin1(hash.result().toHex());
}

// ---------------------------------------------------------------------------
// stage: Extracting
// ---------------------------------------------------------------------------

void PluginInstaller::runExtract()
{
    const Job job = m_jobs.value(m_activeId);

    // The archive must be extracted into a sibling directory so the package root
    // and the .zip itself do not mix.
    const QString target = job.tempDir + QStringLiteral("/pkg");

    const auto result = PluginArchive::extract(job.zipPath, target);

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

    // The index and the bytes must agree. A mismatch means the market record is
    // stale or someone swapped the package — either way, do not install it.
    if (!job.expectedVersion.isEmpty() && parsed->version != job.expectedVersion) {
        fail(QStringLiteral("version-mismatch"),
             QStringLiteral("包内版本 %1 与索引声明 %2 不一致，已拒绝安装")
                 .arg(parsed->version, job.expectedVersion));
        return;
    }

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

    // ---- commit point ----
    if (!QDir().rename(job.packageRoot, target)) {
        if (!backup.isEmpty())
            QDir().rename(backup, target);

        fail(QStringLiteral("permission"),
             QStringLiteral("无法写入插件目录：%1").arg(target));
        return;
    }

    emitLog(QStringLiteral("已写入 %1").arg(target));

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
    entry.origin.kind = job.url.isEmpty() ? QStringLiteral("local")
                                          : QStringLiteral("market");
    entry.origin.url    = job.url.isEmpty() ? QString() : job.url.toString();
    entry.origin.sha256 = job.expectedSha256;
    entry.origin.path   = job.url.isEmpty() ? job.zipPath : QString();

    QString saveError;
    if (!m_registry->upsertAndSave(entry, &saveError)) {
        emitLog(QStringLiteral("状态写入失败：%1").arg(saveError));

        const QString requestId = job.requestId;
        emit finished(requestId, true, QString(),
                      QStringLiteral("插件已写入，但状态记录失败：%1").arg(saveError));
        cleanupTemp();
        m_jobs.remove(requestId);
        m_activeId.clear();
        return;
    }

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
