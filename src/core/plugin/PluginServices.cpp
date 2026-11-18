#include "core/plugin/PluginServices.h"

#include "core/Logger.h"
#include "core/plugin/PluginHost.h"
#include "core/plugin/PluginManifest.h"

#include "contract/IHostApi.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStringList>

namespace {

/// Capability names live in the contract; keeping the checks in one place stops the
/// "declared in one file, checked in another" drift.
bool hasCapability(const PluginManifest &manifest, const char *name)
{
    for (const QString &declared : manifest.permissions) {
        if (declared == QLatin1String(name))
            return true;
    }
    return false;
}

bool wantsClipboard(const PluginManifest &manifest)
{
    return hasCapability(manifest, contract::capability::kClipboardRead)
        || hasCapability(manifest, contract::capability::kClipboardWrite);
}

} // namespace

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

PluginServiceRegistry::PluginServiceRegistry(QObject *parent)
    : QObject(parent)
{
}

void PluginServiceRegistry::registerService(const QString &name, Factory factory,
                                            Available available)
{
    if (name.trimmed().isEmpty() || !factory) {
        Logger::get().warn("PluginServiceRegistry: refused to register a nameless/empty service");
        return;
    }

    Entry entry;
    entry.factory   = std::move(factory);
    entry.available = std::move(available);

    m_services.insert(name.trimmed(), entry);
}

bool PluginServiceRegistry::isRegistered(const QString &name) const
{
    return m_services.contains(name);
}

bool PluginServiceRegistry::isAvailable(const QString &name,
                                        const PluginManifest &manifest) const
{
    const auto it = m_services.constFind(name);
    if (it == m_services.constEnd())
        return false;

    if (!it->available)
        return true;

    return it->available(manifest);
}

QStringList PluginServiceRegistry::names() const
{
    QStringList out = m_services.keys();
    out.sort(Qt::CaseSensitive);
    return out;
}

QStringList PluginServiceRegistry::availableFor(const PluginManifest &manifest) const
{
    QStringList out;
    for (const QString &name : names()) {
        if (isAvailable(name, manifest))
            out.append(name);
    }
    return out;
}

QObject *PluginServiceRegistry::create(const QString &name, PluginHost *host,
                                       const PluginManifest &manifest) const
{
    if (!host)
        return nullptr;

    const auto it = m_services.constFind(name);
    if (it == m_services.constEnd())
        return nullptr;

    if (!isAvailable(name, manifest))
        return nullptr;

    return it->factory(host, manifest);
}

QStringList PluginServiceRegistry::unavailableFor(const PluginManifest &manifest,
                                                  const QStringList &requested) const
{
    QStringList out;
    for (const QString &name : requested) {
        if (!isAvailable(name, manifest))
            out.append(name);
    }
    return out;
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

PluginSettingsService::PluginSettingsService(PluginHost *host, QObject *parent)
    : QObject(parent)
    , m_host(host)
{
}

QVariant PluginSettingsService::get(const QString &key, const QVariant &fallback) const
{
    return m_host ? m_host->setting(key, fallback) : fallback;
}

bool PluginSettingsService::set(const QString &key, const QVariant &value)
{
    return m_host ? m_host->setSetting(key, value) : false;
}

// ---------------------------------------------------------------------------
// notify
// ---------------------------------------------------------------------------

PluginNotifyService::PluginNotifyService(PluginHost *host, QObject *parent)
    : QObject(parent)
    , m_host(host)
{
}

void PluginNotifyService::show(const QString &text)
{
    if (m_host)
        m_host->toast(text);
}

// ---------------------------------------------------------------------------
// logger
// ---------------------------------------------------------------------------

PluginLoggerService::PluginLoggerService(PluginHost *host, QObject *parent)
    : QObject(parent)
    , m_host(host)
{
}

void PluginLoggerService::debug(const QString &message) { write(QStringLiteral("debug"), message); }
void PluginLoggerService::info(const QString &message)  { write(QStringLiteral("info"), message); }
void PluginLoggerService::warn(const QString &message)  { write(QStringLiteral("warn"), message); }
void PluginLoggerService::error(const QString &message) { write(QStringLiteral("error"), message); }

void PluginLoggerService::write(const QString &level, const QString &message)
{
    if (m_host)
        m_host->log(level, message);
}

// ---------------------------------------------------------------------------
// clipboard
// ---------------------------------------------------------------------------

PluginClipboardService::PluginClipboardService(bool canRead, bool canWrite, QObject *parent)
    : QObject(parent)
    , m_canRead(canRead)
    , m_canWrite(canWrite)
{
}

QString PluginClipboardService::read() const
{
    if (!m_canRead)
        return QString();

    // QClipboard needs a QGuiApplication. Tests run with QCoreApplication, so this
    // must stay lazy and null-safe rather than cached at construction.
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return QGuiApplication::clipboard()->text();

    return QString();
}

bool PluginClipboardService::write(const QString &text)
{
    if (!m_canWrite)
        return false;

    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        QGuiApplication::clipboard()->setText(text);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

PluginOpenService::PluginOpenService(PluginHost *host, QObject *parent)
    : QObject(parent)
    , m_host(host)
{
}

bool PluginOpenService::url(const QString &value)
{
    // Deliberately reusing PluginHost::openPath: one restriction list, not two.
    return m_host ? m_host->openPath(value) : false;
}
