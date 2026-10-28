#include "ui/PluginBridge.h"

#include "core/plugin/PluginManager.h"

PluginBridge::PluginBridge(PluginManager *manager, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
    if (!m_manager)
        return;

    connect(m_manager, &PluginManager::viewChanged,
            this, &PluginBridge::listsChanged);
    connect(m_manager, &PluginManager::navigationChanged,
            this, &PluginBridge::navigationChanged);
    connect(m_manager, &PluginManager::toast,
            this, &PluginBridge::toast);
}

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------

QVariantList PluginBridge::installedList() const
{
    return m_manager ? m_manager->installedRows() : QVariantList{};
}

QVariantList PluginBridge::availableList() const
{
    return m_manager ? m_manager->availableRows() : QVariantList{};
}

QVariantList PluginBridge::categories() const
{
    // Category chips are derived from market entries. With no catalog (M0) only the
    // "全部" chip is meaningful, and PluginsPage always prepends that itself.
    return {};
}

QVariantList PluginBridge::navigationEntries() const
{
    return m_manager ? m_manager->navigationEntries() : QVariantList{};
}

bool PluginBridge::isOffline() const
{
    return m_manager ? m_manager->isOffline() : false;
}

QString PluginBridge::fetchedAtText() const
{
    return m_manager ? m_manager->fetchedAtText() : QString();
}

// ---------------------------------------------------------------------------
// intents
// ---------------------------------------------------------------------------

void PluginBridge::refresh()
{
    if (!m_manager)
        return;

    // M0 has no remote catalog, so "refresh" only re-scans the local directory.
    // Emitting loadingChanged around it keeps the UI's skeleton path exercised.
    m_loading = true;
    emit loadingChanged();

    m_manager->discoverUnregistered();

    m_loading = false;
    emit loadingChanged();
    emit listsChanged();
}

void PluginBridge::install(const QString &id)
{
    if (!m_manager)
        return;
    // M2 wires PluginInstaller; until then this must not pretend to work.
    emit toast(QStringLiteral("error"),
               QStringLiteral("安装功能尚未接入（%1）").arg(id));
}

void PluginBridge::installFromZip(const QString &zipPath)
{
    Q_UNUSED(zipPath)
    emit toast(QStringLiteral("error"), QStringLiteral("侧载功能尚未接入"));
}

void PluginBridge::cancelInstall(const QString &requestId)
{
    Q_UNUSED(requestId)
    emit toast(QStringLiteral("error"), QStringLiteral("没有正在进行的安装"));
}

void PluginBridge::setEnabled(const QString &id, bool enabled)
{
    if (m_manager)
        m_manager->setEnabled(id, enabled);
}

void PluginBridge::uninstall(const QString &id)
{
    if (m_manager)
        m_manager->requestUninstall(id);
}

void PluginBridge::openPluginDir(const QString &id)
{
    if (m_manager)
        m_manager->openPluginDir(id);
}

QObject *PluginBridge::acquirePage(const QString &key)
{
    return m_manager ? m_manager->acquirePage(key) : nullptr;
}
