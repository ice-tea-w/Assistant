#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>

struct PluginManifest;
class PluginHost;

/// One host service a plugin can ask for by name.
///
/// The shape is Cordis's `inject`/service lookup, reduced to what this host needs:
///
///   * the host registers a factory per service name;
///   * a plugin declares `inject: ["clipboard"]` in `plugin.json`, and the manager
///     refuses to enable it when one of those names is unavailable (clear problem,
///     not a half-working plugin);
///   * `host.service("clipboard")` returns a **per-plugin view** object, never a
///     host internal. Availability may depend on the manifest (e.g. clipboard needs
///     `clipboard.read`/`clipboard.write`), which is the one place permissions are
///     actually enforced rather than merely displayed.
class PluginServiceRegistry : public QObject
{
    Q_OBJECT

public:
    using Factory   = std::function<QObject *(PluginHost *, const PluginManifest &)>;
    using Available = std::function<bool(const PluginManifest &)>;

    explicit PluginServiceRegistry(QObject *parent = nullptr);

    /// @param available optional predicate over the manifest; empty means "always"
    void registerService(const QString &name, Factory factory,
                         Available available = Available());

    bool        isRegistered(const QString &name) const;
    bool        isAvailable(const QString &name, const PluginManifest &manifest) const;
    QStringList names() const;                    ///< sorted
    QStringList availableFor(const PluginManifest &manifest) const;

    /// Build a new per-plugin view. Caller owns it (PluginHost parents it).
    /// @return nullptr when the name is unknown or unavailable for this manifest
    QObject *create(const QString &name, PluginHost *host,
                    const PluginManifest &manifest) const;

    /// Subset of @p requested that cannot be satisfied, including unknown names.
    QStringList unavailableFor(const PluginManifest &manifest,
                               const QStringList &requested) const;

private:
    struct Entry
    {
        Factory   factory;
        Available available;
    };

    QHash<QString, Entry> m_services;
};

// ---------------------------------------------------------------------------
// Built-in service views
// ---------------------------------------------------------------------------
//
// Each one is a thin adapter over PluginHost. They exist so a plugin gets a named
// capability surface instead of the whole host, and so capabilities can be gated
// per service rather than per method.

class PluginSettingsService : public QObject
{
    Q_OBJECT

public:
    explicit PluginSettingsService(PluginHost *host, QObject *parent = nullptr);

    Q_INVOKABLE QVariant get(const QString &key,
                             const QVariant &fallback = QVariant()) const;
    Q_INVOKABLE bool set(const QString &key, const QVariant &value);

private:
    PluginHost *m_host = nullptr;
};

class PluginNotifyService : public QObject
{
    Q_OBJECT

public:
    explicit PluginNotifyService(PluginHost *host, QObject *parent = nullptr);

    Q_INVOKABLE void show(const QString &text);

private:
    PluginHost *m_host = nullptr;
};

class PluginLoggerService : public QObject
{
    Q_OBJECT

public:
    explicit PluginLoggerService(PluginHost *host, QObject *parent = nullptr);

    Q_INVOKABLE void debug(const QString &message);
    Q_INVOKABLE void info(const QString &message);
    Q_INVOKABLE void warn(const QString &message);
    Q_INVOKABLE void error(const QString &message);

private:
    void write(const QString &level, const QString &message);

    PluginHost *m_host = nullptr;
};

/// Clipboard access, gated by `permissions: ["clipboard.read"] / ["clipboard.write"]`.
/// Reads return an empty string and writes return false when the capability is absent.
class PluginClipboardService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool canRead READ canRead CONSTANT)
    Q_PROPERTY(bool canWrite READ canWrite CONSTANT)

public:
    PluginClipboardService(bool canRead, bool canWrite, QObject *parent = nullptr);

    bool canRead() const { return m_canRead; }
    bool canWrite() const { return m_canWrite; }

    Q_INVOKABLE QString read() const;
    Q_INVOKABLE bool    write(const QString &text);

private:
    bool m_canRead = false;
    bool m_canWrite = false;
};

/// Opening URLs / files, routed through the host's existing restriction
/// (https/http/mailto, plus the plugin's own data dir).
class PluginOpenService : public QObject
{
    Q_OBJECT

public:
    explicit PluginOpenService(PluginHost *host, QObject *parent = nullptr);

    Q_INVOKABLE bool url(const QString &value);

private:
    PluginHost *m_host = nullptr;
};
