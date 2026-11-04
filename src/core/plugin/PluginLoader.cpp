#include "core/plugin/PluginLoader.h"

#include "core/Logger.h"
#include "core/plugin/PluginHost.h"

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
    // Pages first: they are children of the same context as the entry object and
    // may still reference `host` through bindings.
    // QPointer: QML may already have destroyed them by deleting the parent slot.
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

    const PageContribution *page = nullptr;
    for (const PageContribution &candidate : it->manifest.pages) {
        if (candidate.id == pageId) {
            page = &candidate;
            break;
        }
    }

    if (!page)
        return fail(QStringLiteral("插件 %1 没有页面 %2").arg(pluginId, pageId));

    const QString path = it->dir + QLatin1Char('/') + page->source;
    if (!QFileInfo::exists(path))
        return fail(QStringLiteral("页面文件不存在：%1").arg(page->source));

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
            out.append(item);
        }
    }

    return out;
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
