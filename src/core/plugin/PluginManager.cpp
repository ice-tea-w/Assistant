#include "core/plugin/PluginManager.h"

#include "core/Logger.h"
#include "core/plugin/PluginCatalog.h"
#include "core/plugin/PluginInstaller.h"
#include "core/plugin/PluginLoader.h"
#include "core/plugin/PluginRegistry.h"
#include "core/plugin/SemVer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QVariantMap>

namespace {

/// Plugin ids become directory names, so anything we are willing to scan must look
/// like an id. This also filters out `PluginHelper`-style stray folders.
bool looksLikePluginId(const QString &name)
{
    if (name.size() < 3 || name.size() > 64)
        return false;
    if (!(name.at(0).isLetterOrNumber()))
        return false;
    for (const QChar c : name) {
        if (!(c.isLower() || c.isDigit() || c == QLatin1Char('.')
              || c == QLatin1Char('_') || c == QLatin1Char('-')))
            return false;
    }
    return true;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------

PluginManager::PluginManager(PluginRegistry *registry, PluginLoader *loader, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_loader(loader)
{
    if (m_loader) {
        // Any load/unload changes what the navigation should show.
        connect(m_loader, &PluginLoader::navigationChanged,
                this, &PluginManager::navigationChanged);
        connect(m_loader, &PluginLoader::loaded, this,
                [this](const QString &) { emit viewChanged(); });
        connect(m_loader, &PluginLoader::unloaded, this,
                [this](const QString &) { emit viewChanged(); });

        // Plugin toasts are relayed verbatim; the UI decides the presentation.
        connect(m_loader, &PluginLoader::toastRequested, this,
                [this](const QString &pluginId, const QString &text) {
                    emit toast(QStringLiteral("info"),
                               QStringLiteral("%1：%2").arg(pluginId, text));
                });
    }
}

void PluginManager::setEngine(QQmlEngine *engine)
{
    if (m_loader)
        m_loader->setEngine(engine);
}

void PluginManager::setInstaller(PluginInstaller *installer)
{
    m_installer = installer;
    if (!m_installer)
        return;

    connect(m_installer, &PluginInstaller::progress, this,
            [this](const QString &requestId, PluginInstaller::Stage stage,
                   int percent, const QString &detail) {
                emit installProgress(requestId, static_cast<int>(stage), percent, detail);
            });

    connect(m_installer, &PluginInstaller::log, this, &PluginManager::installLog);
    connect(m_installer, &PluginInstaller::finished, this, &PluginManager::installFinished);

    // A fresh install is enabled right away: the user just asked for it, and a
    // plugin that lands disabled looks like nothing happened. If loading fails,
    // enablePlugin() records a problem and the row turns red — still honest.
    connect(m_installer, &PluginInstaller::installed, this,
            [this](const QString &pluginId, const QString &version) {
                Logger::get().info("PluginManager: installed {} v{}, enabling",
                                   pluginId.toStdString(), version.toStdString());
                enablePlugin(pluginId);

                auto entry = m_registry ? m_registry->get(pluginId) : std::nullopt;
                if (entry) {
                    entry->enabled = true;
                    QString error;
                    if (!m_registry->upsertAndSave(*entry, &error))
                        emit toast(QStringLiteral("error"), error);
                }

                emit viewChanged();
                emit navigationChanged();
            });
}

void PluginManager::setCatalog(PluginCatalog *catalog)
{
    m_catalog = catalog;
    if (!m_catalog)
        return;

    connect(m_catalog, &PluginCatalog::fetched, this,
            [this](bool, const QString &) {
                emit viewChanged();
            });
}

void PluginManager::refreshCatalog(bool force)
{
    if (!m_catalog) {
        emit toast(QStringLiteral("error"), QStringLiteral("市场目录未接入"));
        return;
    }
    m_catalog->fetch(force ? true : false);
}

void PluginManager::installFromZip(const QString &zipPath)
{
    if (!m_installer) {
        emit toast(QStringLiteral("error"), QStringLiteral("安装功能未接入"));
        return;
    }

    if (m_installer->isBusy()) {
        emit toast(QStringLiteral("error"), QStringLiteral("正在安装另一个插件，请稍候"));
        return;
    }

    if (m_installer->installFromZip(zipPath).isEmpty())
        emit toast(QStringLiteral("error"), QStringLiteral("无法开始安装"));
}

void PluginManager::cancelInstall(const QString &requestId)
{
    if (!m_installer) {
        emit toast(QStringLiteral("error"), QStringLiteral("没有正在进行的安装"));
        return;
    }

    if (!m_installer->cancel(requestId))
        emit toast(QStringLiteral("error"), QStringLiteral("安装已进入最后一步，无法取消"));
}

void PluginManager::setDark(bool dark)
{
    if (m_loader)
        m_loader->setDark(dark);
}

// ---------------------------------------------------------------------------
// manifests
// ---------------------------------------------------------------------------

bool PluginManager::loadManifestFor(const QString &id, PluginManifest *out,
                                    QString *error) const
{
    const auto cached = m_manifests.constFind(id);
    if (cached != m_manifests.constEnd()) {
        if (out)
            *out = *cached;
        return true;
    }

    if (!m_registry) {
        if (error)
            *error = QStringLiteral("注册表不可用");
        return false;
    }

    const QString path = m_registry->pluginDir(id) + QStringLiteral("/plugin.json");
    const QByteArray raw = readAll(path);

    if (raw.isEmpty()) {
        if (error)
            *error = QStringLiteral("找不到 plugin.json：%1").arg(path);
        return false;
    }

    QString parseError;
    const auto manifest = PluginManifest::parse(raw, &parseError);
    if (!manifest) {
        if (error)
            *error = parseError;
        return false;
    }

    if (manifest->id != id) {
        if (error) {
            *error = QStringLiteral("目录名与清单 id 不一致：目录 %1，清单 %2")
                         .arg(id, manifest->id);
        }
        return false;
    }

    m_manifests.insert(id, *manifest);
    if (out)
        *out = *manifest;
    return true;
}

void PluginManager::setProblem(const QString &id, const QString &code,
                               const QString &detail)
{
    if (!m_registry)
        return;

    auto entry = m_registry->get(id);
    if (!entry)
        return;

    entry->problem.code = code;
    entry->problem.detail = detail;
    // A plugin with a problem must not stay enabled, otherwise the next start
    // tries to load it again and fails the same way.
    entry->enabled = false;

    QString error;
    if (!m_registry->upsertAndSave(*entry, &error))
        Logger::get().warn("PluginManager: cannot persist problem for {}", id.toStdString());

    Logger::get().warn("PluginManager: {} -> {} ({})",
                       id.toStdString(), code.toStdString(), detail.toStdString());

    emit viewChanged();
}

// ---------------------------------------------------------------------------
// discovery
// ---------------------------------------------------------------------------

int PluginManager::discoverUnregistered()
{
    if (!m_registry)
        return 0;

    QDir root(m_registry->rootDir());
    if (!root.exists())
        return 0;

    // Only real directories with a plugin.json; dot-prefixed bookkeeping dirs and
    // leftover `.bak` / `.trash` staging are skipped by looksLikePluginId + suffix.
    const QStringList candidates = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    int added = 0;
    for (const QString &name : candidates) {
        if (name.startsWith(QLatin1Char('.')))
            continue;
        if (name.endsWith(QLatin1String(".bak")))
            continue;
        if (!looksLikePluginId(name))
            continue;

        if (m_registry->contains(name))
            continue;

        const QString manifestPath = m_registry->pluginDir(name) + QStringLiteral("/plugin.json");
        if (!QFileInfo::exists(manifestPath))
            continue;

        QString parseError;
        const auto manifest = PluginManifest::parse(readAll(manifestPath), &parseError);
        if (!manifest) {
            Logger::get().warn("PluginManager: skip {} — {}", name.toStdString(),
                               parseError.toStdString());
            continue;
        }

        if (manifest->id != name) {
            Logger::get().warn("PluginManager: skip {} — manifest id is {}",
                               name.toStdString(), manifest->id.toStdString());
            continue;
        }

        InstalledPlugin entry;
        entry.id          = name;
        entry.version     = manifest->version;
        entry.installedAt = QDateTime::currentDateTimeUtc();
        // Hand-dropped plugins are not enabled automatically: the user should see
        // them appear and decide. (Also keeps a broken drop from breaking startup.)
        entry.enabled     = false;
        entry.origin.kind = QStringLiteral("local");
        entry.origin.path = m_registry->pluginDir(name);

        QString saveError;
        if (!m_registry->upsertAndSave(entry, &saveError)) {
            Logger::get().warn("PluginManager: cannot register {} — {}", name.toStdString(),
                               saveError.toStdString());
            continue;
        }

        m_manifests.insert(name, *manifest);
        ++added;
        Logger::get().info("PluginManager: discovered local plugin {}", name.toStdString());
    }

    if (added > 0)
        emit viewChanged();

    return added;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool PluginManager::enablePlugin(const QString &id)
{
    if (!m_registry || !m_loader)
        return false;

    if (m_loader->isLoaded(id))
        return true;

    PluginManifest manifest;
    QString error;
    if (!loadManifestFor(id, &manifest, &error)) {
        setProblem(id, QStringLiteral("invalid-manifest"), error);
        return false;
    }

    if (manifest.isNativeRuntime()) {
        setProblem(id, QStringLiteral("invalid-manifest"),
                   QStringLiteral("本版本不支持原生插件"));
        return false;
    }

    // Environment gate: a plugin that declares a range we do not satisfy is a
    // compatibility problem, not a load failure — the UI should say so.
    if (!manifest.checkEngine(QCoreApplication::applicationVersion(), &error)) {
        setProblem(id, QStringLiteral("incompatible-version"), error);
        return false;
    }

    if (!m_loader->load(manifest, m_registry->pluginDir(id), &error)) {
        setProblem(id, QStringLiteral("load-failed"), error);
        return false;
    }

    // Clear any stale problem now that it actually loads.
    auto entry = m_registry->get(id);
    if (entry && !entry->problem.isNull()) {
        entry->problem = InstalledPlugin::Problem{};
        m_registry->upsertAndSave(*entry);
    }

    return true;
}

void PluginManager::loadEnabledAtStartup()
{
    if (!m_registry || !m_loader)
        return;

    discoverUnregistered();

    const QStringList ids = m_registry->ids();
    for (const QString &id : ids) {
        const auto entry = m_registry->get(id);
        if (!entry)
            continue;

        if (!entry->enabled)
            continue;

        // A previously recorded problem is retried once at startup: the user may
        // have fixed the plugin files by hand.
        enablePlugin(id);
    }

    // Populate the market half. Seed-only mode is synchronous, so the list is
    // complete before the window is shown (no "empty then pops in" flicker).
    refreshCatalog(false);

    emit viewChanged();
    emit navigationChanged();
}

void PluginManager::unloadAll()
{
    if (m_loader)
        m_loader->unloadAll();
}

// ---------------------------------------------------------------------------
// intents
// ---------------------------------------------------------------------------

void PluginManager::setEnabled(const QString &id, bool enabled)
{
    if (!m_registry || !m_loader)
        return;

    auto entry = m_registry->get(id);
    if (!entry) {
        emit toast(QStringLiteral("error"), QStringLiteral("插件未安装：%1").arg(id));
        return;
    }

    if (!enabled) {
        m_loader->unload(id);
        entry->enabled = false;
        // Disabling is an explicit user decision: clear the problem marker so the
        // row reads "已停用" rather than a stale "不可用".
        entry->problem = InstalledPlugin::Problem{};

        QString error;
        if (!m_registry->upsertAndSave(*entry, &error))
            emit toast(QStringLiteral("error"), error);

        emit viewChanged();
        return;
    }

    // Enabling: load first, persist only on success (enablePlugin persists problems).
    if (!enablePlugin(id)) {
        emit toast(QStringLiteral("error"), QStringLiteral("启用失败，详情见插件卡片"));
        return;
    }

    entry = m_registry->get(id);
    if (entry) {
        entry->enabled = true;
        entry->problem = InstalledPlugin::Problem{};
        QString error;
        if (!m_registry->upsertAndSave(*entry, &error))
            emit toast(QStringLiteral("error"), error);
    }

    emit viewChanged();
}

void PluginManager::requestUninstall(const QString &id)
{
    if (!m_registry || !m_loader)
        return;

    const auto entry = m_registry->get(id);
    if (!entry) {
        emit toast(QStringLiteral("error"), QStringLiteral("插件未安装：%1").arg(id));
        return;
    }

    if (entry->origin.kind == QLatin1String("bundled")) {
        emit toast(QStringLiteral("error"), QStringLiteral("内置插件不可卸载"));
        return;
    }

    // Order matters: the QML object must be gone before its files are deleted.
    m_loader->unload(id);
    m_manifests.remove(id);

    const QString dir = m_registry->pluginDir(id);
    bool removed = QDir(dir).removeRecursively();

    if (!removed) {
        // Files may be locked (a plugin dropped a handle, or the user has the folder
        // open). Move the directory aside and let the next start clean it up.
        const QString trash = m_registry->trashDir() + QLatin1Char('/') + id
            + QLatin1Char('-')
            + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));

        QDir().mkpath(m_registry->trashDir());

        if (QDir().rename(dir, trash)) {
            removed = true;
            Logger::get().warn("PluginManager: {} moved to trash ({})", id.toStdString(),
                               trash.toStdString());
        } else {
            Logger::get().error("PluginManager: cannot delete {}", dir.toStdString());
        }
    }

    if (!removed) {
        emit toast(QStringLiteral("error"),
                   QStringLiteral("无法删除插件目录，可能被占用：%1").arg(dir));
        return;
    }

    QString error;
    if (!m_registry->removeAndSave(id, &error)) {
        emit toast(QStringLiteral("error"), error);
        return;
    }

    emit viewChanged();
    emit navigationChanged();
}

void PluginManager::openPluginDir(const QString &id)
{
    if (!m_registry)
        return;

    const QString dir = id.isEmpty() ? m_registry->rootDir() : m_registry->pluginDir(id);

    if (!QFileInfo::exists(dir)) {
        emit toast(QStringLiteral("error"), QStringLiteral("目录不存在：%1").arg(dir));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// ---------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------

QVariantList PluginManager::rowsFrom(const QStringList &ids) const
{
    QVariantList out;

    for (const QString &id : ids) {
        if (!m_registry)
            break;

        const auto entry = m_registry->get(id);
        if (!entry)
            continue;

        PluginManifest manifest;
        const bool haveManifest = loadManifestFor(id, &manifest, nullptr);

        QVariantMap row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("name"),
                   haveManifest && !manifest.name.isEmpty() ? manifest.name : id);
        row.insert(QStringLiteral("version"), entry->version);
        row.insert(QStringLiteral("summary"), haveManifest ? manifest.description : QString());
        row.insert(QStringLiteral("icon"), haveManifest ? manifest.icon : QString());
        row.insert(QStringLiteral("enabled"), entry->enabled);
        row.insert(QStringLiteral("percent"), 0);

        QStringList permissions;
        if (haveManifest)
            permissions = manifest.permissions;
        row.insert(QStringLiteral("permissions"), permissions);

        // State is decided here, never in QML (LLD §9.3).
        QString state;
        QString badge;
        QString badgeKind;

        if (!entry->problem.isNull()) {
            state = QStringLiteral("problem");
            badge = QStringLiteral("不可用");
            badgeKind = QStringLiteral("red");
        } else if (entry->origin.kind == QLatin1String("bundled")) {
            state = QStringLiteral("bundled");
            badge = QStringLiteral("内置");
            badgeKind = QStringLiteral("gray");
        } else {
            state = QStringLiteral("installed");
            if (!entry->enabled) {
                badge = QStringLiteral("已停用");
                badgeKind = QStringLiteral("gray");
            }
        }

        row.insert(QStringLiteral("state"), state);
        row.insert(QStringLiteral("badge"), badge);
        row.insert(QStringLiteral("badgeKind"), badgeKind);
        row.insert(QStringLiteral("problem"),
                   entry->problem.isNull() ? QString() : entry->problem.detail);

        out.append(row);
    }

    return out;
}

QVariantList PluginManager::installedRows() const
{
    if (!m_registry)
        return {};
    return rowsFrom(m_registry->ids());
}

QVariantList PluginManager::availableRows() const
{
    if (!m_catalog || !m_registry)
        return {};

    QVariantList out;

    const QList<MarketEntry> entries = m_catalog->entries();
    for (const MarketEntry &entry : entries) {
        // Installed plugins live in the first group; showing them twice is noise.
        if (m_registry->contains(entry.id))
            continue;

        QVariantMap row;
        row.insert(QStringLiteral("id"), entry.id);
        row.insert(QStringLiteral("name"), entry.name);
        row.insert(QStringLiteral("version"), entry.latest.version);
        row.insert(QStringLiteral("summary"), entry.summary);
        row.insert(QStringLiteral("icon"), entry.icon);
        row.insert(QStringLiteral("enabled"), false);
        row.insert(QStringLiteral("percent"), 0);
        row.insert(QStringLiteral("permissions"), QStringList());
        row.insert(QStringLiteral("categories"), entry.categories);
        row.insert(QStringLiteral("author"), entry.author);
        row.insert(QStringLiteral("description"), entry.description);
        row.insert(QStringLiteral("changelog"), entry.latest.changelog);
        row.insert(QStringLiteral("downloads"), static_cast<qlonglong>(entry.downloads));

        // State for a not-installed entry. "incompatible" is decided from the
        // index-side minAppVersion so we can grey the button without downloading.
        QString state = QStringLiteral("not-installed");
        QString badge;
        QString badgeKind;
        QString problem;

        const QString minApp = entry.latest.minAppVersion;
        if (!minApp.isEmpty()) {
            const auto app = semver::parse(QStringView(QCoreApplication::applicationVersion()));
            const auto need = semver::parse(QStringView(minApp));
            if (app && need && semver::compare(*app, *need) < 0) {
                state = QStringLiteral("incompatible");
                badge = QStringLiteral("需 ") + minApp;
                badgeKind = QStringLiteral("orange");
                problem = QStringLiteral("需要 Assistant %1，当前 %2")
                              .arg(minApp, QCoreApplication::applicationVersion());
            }
        }

        if (badge.isEmpty() && !entry.tags.isEmpty()) {
            const QString tag = entry.tags.first();
            badge = tag;
            badgeKind = (tag == QStringLiteral("官方")) ? QStringLiteral("accent")
                                                       : QStringLiteral("orange");
        }

        row.insert(QStringLiteral("state"), state);
        row.insert(QStringLiteral("badge"), badge);
        row.insert(QStringLiteral("badgeKind"), badgeKind);
        row.insert(QStringLiteral("problem"), problem);

        out.append(row);
    }

    return out;
}

QVariantList PluginManager::navigationEntries() const
{
    if (!m_loader)
        return {};
    return m_loader->navigationEntries();
}

QObject *PluginManager::acquirePage(const QString &key)
{
    if (!m_loader)
        return nullptr;

    // key format is "<pluginId>:<pageId>"; plugin ids may contain dots but never colons.
    const int sep = key.indexOf(QLatin1Char(':'));
    if (sep <= 0 || sep >= key.size() - 1)
        return nullptr;

    const QString pluginId = key.left(sep);
    const QString pageId   = key.mid(sep + 1);

    QString error;
    QObject *item = m_loader->createPage(pluginId, pageId, &error);

    if (!item && !error.isEmpty()) {
        Logger::get().warn("PluginManager: page {} failed — {}", key.toStdString(),
                           error.toStdString());
        emit toast(QStringLiteral("error"), error);
    }

    return item;
}

bool PluginManager::isOffline() const
{
    return m_catalog ? m_catalog->isOffline() : false;
}

QString PluginManager::fetchedAtText() const
{
    return m_catalog ? m_catalog->fetchedAtText() : QString();
}

QStringList PluginManager::categories() const
{
    return m_catalog ? m_catalog->categories() : QStringList();
}
