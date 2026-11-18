#pragma once

#include <QObject>
#include <QString>

#include <functional>

/// One revocable registration, exposed to plugin QML as an object.
///
/// Why a class instead of just "remove the thing later": plugin code registers into
/// host-owned structures (navigation pages, commands, …). Without a handle the only
/// way out is a global "remove everything of this plugin" sweep, which cannot express
/// "stop this one page now, keep the rest" and cannot run cleanup in the right order.
///
/// Contract:
///   * `dispose()` is **idempotent** — second call does nothing and returns quietly;
///   * a throw from the disposer is caught and logged, it must not escape into QML;
///   * disposal order inside a group is **LIFO** (last registered, first released),
///     which is what makes one registration able to depend on an earlier one.
///
/// Modelled on Cordis's `ctx.effect(() => dispose)`, but with only the one primitive
/// this code base needs.
class PluginDisposable : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool disposed READ isDisposed NOTIFY disposedChanged)

public:
    /// @param pluginId for logging only; empty means "host internal"
    PluginDisposable(QString pluginId, std::function<void()> disposer,
                     QObject *parent = nullptr);
    ~PluginDisposable() override;

    bool isDisposed() const { return m_disposed; }

    /// Release the registration. Idempotent; never throws.
    Q_INVOKABLE void dispose();

signals:
    void disposedChanged();

private:
    QString m_pluginId;
    std::function<void()> m_disposer;
    bool m_disposed = false;
};

/// LIFO collection of disposables owned by one plugin.
///
/// The loader creates one per loaded plugin and disposes it **before** destroying the
/// QML objects, so a disposer can still touch live QML state (e.g. remove a nav entry
/// while the plugin's function is still callable).
///
/// QObject-derived so the handles can be parented to it: when the group dies, any
/// handle the plugin forgot to keep alive goes with it.
class DisposalGroup : public QObject
{
public:
    explicit DisposalGroup(QString pluginId, QObject *parent = nullptr);
    ~DisposalGroup() override;

    DisposalGroup(const DisposalGroup &) = delete;
    DisposalGroup &operator=(const DisposalGroup &) = delete;

    /// Take ownership of a disposer and return the handle QML holds.
    PluginDisposable *add(std::function<void()> disposer);

    /// Dispose everything, newest first. One failing disposer does not stop the rest.
    /// @return how many were actually disposed
    int disposeAll();

    int count() const { return static_cast<int>(m_items.size()); }
    bool isEmpty() const { return m_items.isEmpty(); }
    const QString &pluginId() const { return m_pluginId; }

private:
    QString m_pluginId;
    QList<PluginDisposable *> m_items;
};
