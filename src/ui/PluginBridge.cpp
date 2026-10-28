#include "ui/PluginBridge.h"

#include "core/plugin/PluginManager.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QUrl>

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

    connect(m_manager, &PluginManager::installProgress,
            this, &PluginBridge::installProgress);
    connect(m_manager, &PluginManager::installLog,
            this, &PluginBridge::installLog);
    connect(m_manager, &PluginManager::installFinished,
            this, &PluginBridge::installFinished);
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
    // Derived from the market index, never hardcoded in QML.
    QVariantList out;
    if (!m_manager)
        return out;

    const QStringList names = m_manager->categories();
    for (const QString &name : names)
        out.append(name);

    return out;
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

    m_loading = true;
    emit loadingChanged();

    // Rescan the local directory *and* re-read the market index. In seed-only mode
    // the catalog part is synchronous, so `loading` is honest for both paths.
    m_manager->discoverUnregistered();
    m_manager->refreshCatalog(true);

    m_loading = false;
    emit loadingChanged();
    emit listsChanged();
    emit catalogChanged();
}

void PluginBridge::install(const QString &id)
{
    if (!m_manager)
        return;
    // Market lookups need PluginCatalog (M1). Side-loading is available now.
    emit toast(QStringLiteral("error"),
               QStringLiteral("从市场安装尚未接入（%1），可先用「从文件安装」").arg(id));
}

void PluginBridge::pickAndInstall()
{
    if (!m_manager)
        return;

    const QString startDir =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

    const QString path = QFileDialog::getOpenFileName(
        nullptr,
        QStringLiteral("选择插件包"),
        startDir,
        QStringLiteral("插件包 (*.zip);;所有文件 (*)"));

    if (path.isEmpty())
        return;    // user cancelled the picker — not an error

    m_manager->installFromZip(path);
}

void PluginBridge::installFromZip(const QString &zipPath)
{
    if (m_manager)
        m_manager->installFromZip(zipPath);
}

void PluginBridge::cancelInstall(const QString &requestId)
{
    if (m_manager)
        m_manager->cancelInstall(requestId);
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
