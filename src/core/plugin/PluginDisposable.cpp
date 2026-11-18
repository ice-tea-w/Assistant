#include "core/plugin/PluginDisposable.h"

#include "core/Logger.h"

PluginDisposable::PluginDisposable(QString pluginId, std::function<void()> disposer,
                                   QObject *parent)
    : QObject(parent)
    , m_pluginId(std::move(pluginId))
    , m_disposer(std::move(disposer))
{
}

PluginDisposable::~PluginDisposable()
{
    // Deliberately **not** running the disposer here: during host shutdown the states
    // it would touch are already gone, and the loader has disposed the group first.
    // A handle that dies without dispose() therefore means "already released".
}

void PluginDisposable::dispose()
{
    if (m_disposed)
        return;

    // Flip first: a disposer that re-enters dispose() (directly or through host code)
    // must see "already disposed" instead of recursing.
    m_disposed = true;

    if (m_disposer) {
        try {
            m_disposer();
        } catch (const std::exception &e) {
            Logger::get().warn("PluginDisposable[{}]: disposer threw — {}",
                               m_pluginId.toStdString(), e.what());
        } catch (...) {
            Logger::get().warn("PluginDisposable[{}]: disposer threw (unknown)",
                               m_pluginId.toStdString());
        }
    }

    emit disposedChanged();
}

// ---------------------------------------------------------------------------
// DisposalGroup
// ---------------------------------------------------------------------------

DisposalGroup::DisposalGroup(QString pluginId, QObject *parent)
    : QObject(parent)
    , m_pluginId(std::move(pluginId))
{
}

DisposalGroup::~DisposalGroup()
{
    // Dispose before the handles go away: a disposer may still need the handle's
    // identity (e.g. to unregister by pointer). Handles themselves are children,
    // so ~QObject deletes them right after this returns.
    disposeAll();
}

PluginDisposable *DisposalGroup::add(std::function<void()> disposer)
{
    auto *handle = new PluginDisposable(m_pluginId, std::move(disposer), this);
    m_items.append(handle);
    return handle;
}

int DisposalGroup::disposeAll()
{
    int disposed = 0;

    // Newest first: mirrors "the thing registered last may depend on the thing before it".
    while (!m_items.isEmpty()) {
        PluginDisposable *handle = m_items.takeLast();
        if (!handle)
            continue;
        if (!handle->isDisposed()) {
            handle->dispose();
            ++disposed;
        }
    }

    return disposed;
}
