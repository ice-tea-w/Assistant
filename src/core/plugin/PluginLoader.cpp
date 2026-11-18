#include "core/plugin/PluginLoader.h"

#include "core/Logger.h"
#include "core/plugin/PluginDisposable.h"
#include "core/plugin/PluginExtensionRegistry.h"
#include "core/plugin/PluginHost.h"
#include "core/plugin/PluginServices.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantMap>

PluginLoader::PluginLoader(QObject *parent)
    : QObject(parent)
{
}

PluginLoader::~PluginLoader()
{
    // Never let plugin objects outlive the engine.
    unloadAll();
}

void PluginLoader::setEngine(QQmlEngine *engine)
{
    m_engine = engine;
}

void PluginLoader::setExtensionRegistry(PluginExtensionRegistry *registry)
{
    m_extensions = registry;
}

void PluginLoader::setServiceRegistry(PluginServiceRegistry *registry)
{
    m_services = registry;
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

bool PluginLoader::load(const PluginManifest &manifest, const QString &pluginDir,
                        QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (!m_engine)
        return fail(QStringLiteral("尚未设置 QML engine"));

    if (manifest.id.isEmpty())
        return fail(QStringLiteral("清单缺少 id"));

    if (m_loaded.contains(manifest.id))
        return fail(QStringLiteral("插件已加载：%1").arg(manifest.id));

    // Runtime check comes before `main`, because a native plugin legitimately has
    // no `main` — reporting "missing main" for it would be misleading.
    if (manifest.isNativeRuntime())
        return fail(QStringLiteral("本版本不支持原生插件（runtime=native）"));

    if (manifest.main.isEmpty())
        return fail(QStringLiteral("清单缺少 main"));

    const QString entryPath = pluginDir + QLatin1Char('/') + manifest.main;
    if (!QFileInfo::exists(entryPath))
        return fail(QStringLiteral("入口文件不存在：%1").arg(manifest.main));

    auto *component = new QQmlComponent(m_engine, QUrl::fromLocalFile(entryPath));

    if (component->isError()) {
        QStringList messages;
        const auto errors = component->errors();
        for (const QQmlError &e : errors) {
            // "file:///…/main.qml:12:5: Unexpected token '}'" is the useful part;
            // keep Qt's own line/column formatting rather than re-deriving it.
            messages.append(e.toString());
        }
        delete component;
        return fail(messages.isEmpty() ? QStringLiteral("QML 编译失败")
                                       : messages.join(QLatin1Char('\n')));
    }

    auto *context = new QQmlContext(m_engine->rootContext());

    const QString dataDir = pluginDir + QStringLiteral("/data");
    auto *host = new PluginHost(manifest.id, manifest.name, manifest.version, dataDir, this);
    host->setAppVersion(QCoreApplication::applicationVersion());

    // Per-plugin lifecycle group: created before QML runs, so anything the plugin
    // registers from Component.onCompleted is already tracked.
    auto *disposal = new DisposalGroup(manifest.id, this);
    host->setManifest(manifest);
    host->setExtensionRegistry(m_extensions);
    host->setServiceRegistry(m_services);
    host->setDisposalGroup(disposal);
    if (m_services)
        host->setAvailableServices(m_services->availableFor(manifest));

    context->setContextProperty(QStringLiteral("host"), host);

    QObject *object = component->create(context);
    if (!object) {
        QStringList messages;
        const auto errors = component->errors();
        for (const QQmlError &e : errors)
            messages.append(e.toString());

        delete host;
        delete context;
        delete component;
        delete disposal;

        return fail(messages.isEmpty() ? QStringLiteral("QML 实例化失败")
                                       : messages.join(QLatin1Char('\n')));
    }

    LoadedPlugin entry;
    entry.manifest  = manifest;
    entry.dir       = pluginDir;
    entry.object    = object;
    entry.component = component;
    entry.context   = context;
    entry.host      = host;
    entry.disposal  = disposal;

    m_loaded.insert(manifest.id, entry);
    wireHost(m_loaded[manifest.id]);

    Logger::get().info("PluginLoader: loaded {} ({})",
                       manifest.id.toStdString(), entryPath.toStdString());

    emit loaded(manifest.id);
    emit navigationChanged();
    return true;
}

void PluginLoader::wireHost(LoadedPlugin &plugin)
{
    auto *host = static_cast<PluginHost *>(plugin.host);
    if (!host)
        return;

    const QString id = plugin.manifest.id;
    connect(host, &PluginHost::toastRequested, this,
            [this, id](const QString &text) { emit toastRequested(id, text); });
}

// ---------------------------------------------------------------------------
// unload
// ---------------------------------------------------------------------------

void PluginLoader::destroy(LoadedPlugin &plugin)
{
    // 1. Release runtime registrations **first**: a disposer may still want to touch
    //    the plugin's Qt state (e.g. call a JS cleanup function), and it must be gone
    //    from the host's registries before the QML objects die.
    if (plugin.disposal) {
        const int disposed = plugin.disposal->disposeAll();
        if (disposed > 0) {
            Logger::get().info("PluginLoader: disposed {} registration(s) of {}",
                               disposed, plugin.manifest.id.toStdString());
        }
        delete plugin.disposal;
        plugin.disposal = nullptr;
    }

    // Safety net: a plugin that registered through a path we do not track still
    // must not survive its own unload.
    if (m_extensions)
        m_extensions->removeAllOf(plugin.manifest.id);

    // 2. Pages: they are children of the same context as the entry object and may
    //    still reference `host` through bindings.
    //    QPointer: QML may already have destroyed them by deleting the parent slot.
    for (auto it = plugin.pageItems.begin(); it != plugin.pageItems.end(); ++it)
        delete it.value().data();
    plugin.pageItems.clear();

    for (auto it = plugin.pageComponents.begin(); it != plugin.pageComponents.end(); ++it)
        delete static_cast<QQmlComponent *>(it.value());
    plugin.pageComponents.clear();

    // Then the entry object, the component and finally the context + host.
    delete plugin.object;
    plugin.object = nullptr;

    delete static_cast<QQmlComponent *>(plugin.component);
    plugin.component = nullptr;

    delete static_cast<QQmlContext *>(plugin.context);
    plugin.context = nullptr;

    delete static_cast<PluginHost *>(plugin.host);
    plugin.host = nullptr;
}

void PluginLoader::destroyPages(const QString &id)
{
    auto it = m_loaded.find(id);
    if (it == m_loaded.end())
        return;

    for (auto p = it->pageItems.begin(); p != it->pageItems.end(); ++p)
        delete p.value().data();
    it->pageItems.clear();

    for (auto c = it->pageComponents.begin(); c != it->pageComponents.end(); ++c)
        delete static_cast<QQmlComponent *>(c.value());
    it->pageComponents.clear();
}

QObject *PluginLoader::createPage(const QString &pluginId, const QString &pageId,
                                  QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };

    if (!m_engine)
        return fail(QStringLiteral("尚未设置 QML engine"));

    auto it = m_loaded.find(pluginId);
    if (it == m_loaded.end())
        return fail(QStringLiteral("插件未加载：%1").arg(pluginId));

    if (it->pageItems.contains(pageId)) {
        QObject *cached = it->pageItems.value(pageId).data();
        if (cached)
            return cached;
        // Dangling (the QML slot destroyed it): drop the record and rebuild.
        it->pageItems.remove(pageId);
        auto comp = it->pageComponents.find(pageId);
        if (comp != it->pageComponents.end()) {
            delete static_cast<QQmlComponent *>(comp.value());
            it->pageComponents.erase(comp);
        }
    }

    const QString source = pageSourceOf(*it, pageId);
    if (source.isEmpty())
        return fail(QStringLiteral("插件 %1 没有页面 %2").arg(pluginId, pageId));

    // Runtime contributions may carry an absolute file URL; manifest pages are always
    // plugin-relative. Both are accepted, and the same path-safety rule applies.
    const bool absolute = source.startsWith(QStringLiteral("file:"))
                          || QFileInfo(source).isAbsolute();
    const QString path = absolute ? QUrl(source).toLocalFile()
                                  : it->dir + QLatin1Char('/') + source;

    if (!QFileInfo::exists(path))
        return fail(QStringLiteral("页面文件不存在：%1").arg(source));

    auto *component = new QQmlComponent(m_engine, QUrl::fromLocalFile(path));

    if (component->isError()) {
        QStringList messages;
        for (const QQmlError &e : component->errors())
            messages.append(e.toString());
        delete component;
        return fail(messages.isEmpty() ? QStringLiteral("页面 QML 编译失败")
                                       : messages.join(QLatin1Char('\n')));
    }

    // Same context as main.qml, which is what makes `host` available here.
    auto *context = static_cast<QQmlContext *>(it->context);

    QObject *item = component->create(context);
    if (!item) {
        QStringList messages;
        for (const QQmlError &e : component->errors())
            messages.append(e.toString());
        delete component;
        return fail(messages.isEmpty() ? QStringLiteral("页面 QML 实例化失败")
                                       : messages.join(QLatin1Char('\n')));
    }

    it->pageItems.insert(pageId, item);
    it->pageComponents.insert(pageId, component);

    Logger::get().info("PluginLoader: created page {}:{}", pluginId.toStdString(),
                       pageId.toStdString());

    return item;
}

void PluginLoader::unload(const QString &id)
{
    auto it = m_loaded.find(id);
    if (it == m_loaded.end())
        return;

    const bool hadNavigation = !it->manifest.pages.isEmpty();

    destroy(*it);
    m_loaded.erase(it);

    Logger::get().info("PluginLoader: unloaded {}", id.toStdString());

    emit unloaded(id);
    if (hadNavigation)
        emit navigationChanged();
}

void PluginLoader::unloadAll()
{
    if (m_loaded.isEmpty())
        return;

    const QStringList ids = loadedIds();
    for (const QString &id : ids) {
        auto it = m_loaded.find(id);
        if (it != m_loaded.end()) {
            destroy(*it);
            m_loaded.erase(it);
        }
    }

    emit navigationChanged();
}

// ---------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------

QStringList PluginLoader::loadedIds() const
{
    QStringList list = m_loaded.keys();
    list.sort(Qt::CaseSensitive);
    return list;
}

bool PluginLoader::isLoaded(const QString &id) const
{
    return m_loaded.contains(id);
}

QObject *PluginLoader::pluginObject(const QString &id) const
{
    const auto it = m_loaded.constFind(id);
    return it == m_loaded.constEnd() ? nullptr : it->object;
}

QVariantList PluginLoader::navigationEntries() const
{
    QVariantList out;

    QStringList ids = m_loaded.keys();
    ids.sort(Qt::CaseSensitive);

    for (const QString &id : ids) {
        // value() copies the struct; it only holds pointers, so this is cheap and
        // avoids QHash::operator[] on a const method.
        const LoadedPlugin plugin = m_loaded.value(id);

        // Static contributions (manifest) first, so they win when a plugin registers
        // the same page id at runtime.
        QStringList seen;
        for (const PageContribution &page : plugin.manifest.pages) {
            QVariantMap item;
            item.insert(QStringLiteral("pluginId"), id);
            item.insert(QStringLiteral("pageId"), page.id);
            item.insert(QStringLiteral("title"), page.title);
            item.insert(QStringLiteral("icon"), page.icon);
            item.insert(QStringLiteral("key"), id + QLatin1Char(':') + page.id);
            item.insert(QStringLiteral("source"),
                        QUrl::fromLocalFile(plugin.dir + QLatin1Char('/') + page.source)
                            .toString());
            item.insert(QStringLiteral("dynamic"), false);
            out.append(item);
            seen.append(page.id);
        }

        // Runtime contributions (`host.addExtension("nav.pages", …)`).
        if (!m_extensions)
            continue;

        const QVariantList dynamic =
            m_extensions->contributions(QStringLiteral("nav.pages"));
        for (const QVariant &entry : dynamic) {
            const QVariantMap row = entry.toMap();
            if (row.value(QStringLiteral("pluginId")).toString() != id)
                continue;

            const QString pageId = row.value(QStringLiteral("pageId")).toString();
            if (pageId.isEmpty() || seen.contains(pageId))
                continue;

            const QString source = row.value(QStringLiteral("source")).toString();
            const bool absolute = source.startsWith(QStringLiteral("file:"))
                                  || QFileInfo(source).isAbsolute();

            QVariantMap item;
            item.insert(QStringLiteral("pluginId"), id);
            item.insert(QStringLiteral("pageId"), pageId);
            item.insert(QStringLiteral("title"), row.value(QStringLiteral("title")));
            item.insert(QStringLiteral("icon"), row.value(QStringLiteral("icon")));
            item.insert(QStringLiteral("key"), id + QLatin1Char(':') + pageId);
            item.insert(QStringLiteral("source"),
                        absolute ? QUrl(source).toString()
                                 : QUrl::fromLocalFile(plugin.dir + QLatin1Char('/') + source)
                                       .toString());
            item.insert(QStringLiteral("dynamic"), true);
            out.append(item);
            seen.append(pageId);
        }
    }

    return out;
}

QString PluginLoader::pageSourceOf(const LoadedPlugin &plugin, const QString &pageId) const
{
    for (const PageContribution &page : plugin.manifest.pages) {
        if (page.id == pageId)
            return page.source;
    }

    if (!m_extensions)
        return QString();

    const QVariantList dynamic = m_extensions->contributions(QStringLiteral("nav.pages"));
    for (const QVariant &entry : dynamic) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("pluginId")).toString() != plugin.manifest.id)
            continue;
        if (row.value(QStringLiteral("pageId")).toString() != pageId)
            continue;
        return row.value(QStringLiteral("source")).toString();
    }

    return QString();
}

// ---------------------------------------------------------------------------
// host-wide facts
// ---------------------------------------------------------------------------

void PluginLoader::setAppVersion(const QString &appVersion)
{
    for (auto it = m_loaded.begin(); it != m_loaded.end(); ++it) {
        if (auto *host = static_cast<PluginHost *>(it->host))
            host->setAppVersion(appVersion);
    }
}

void PluginLoader::setDark(bool dark)
{
    for (auto it = m_loaded.begin(); it != m_loaded.end(); ++it) {
        if (auto *host = static_cast<PluginHost *>(it->host))
            host->setDark(dark);
    }
}
