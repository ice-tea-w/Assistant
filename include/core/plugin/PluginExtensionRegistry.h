#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class DisposalGroup;
class PluginDisposable;

/// One named extension point the host publishes.
///
/// Borrowed from Cordis's idea of "the host declares slots, plugins fill them", but
/// kept to a data model: a point has an id, a title, the keys a contribution must
/// carry, and whether anything actually consumes it today.
///
/// `consumed == false` is deliberate honesty: declaring a point before its consumer
/// exists is allowed (it gives plugin authors a stable name), but the UI and README
/// must say "declared, nothing reads it yet" rather than pretend it works.
struct ExtensionPoint
{
    QString     id;
    QString     title;
    QStringList requiredKeys;
    bool        consumed = true;
};

/// Host-owned registry of extension points and their contributions.
///
/// Invariants:
///   * a contribution belongs to exactly one plugin and one declared point;
///   * every contribution is reachable through a `PluginDisposable`, so the plugin
///     can withdraw it individually and the loader can withdraw all of them on unload;
///   * removing a contribution emits `changed(pointId)` exactly once.
///
/// Thread: main thread only (consumers are QML-facing).
class PluginExtensionRegistry : public QObject
{
    Q_OBJECT

public:
    explicit PluginExtensionRegistry(QObject *parent = nullptr);
    ~PluginExtensionRegistry() override;

    /// Publish a point. Re-declaring the same id updates title/keys instead of failing,
    /// so tests and future modules can adjust without ordering rules.
    bool declare(const QString &id, const QString &title,
                 const QStringList &requiredKeys = QStringList(),
                 bool consumed = true, QString *error = nullptr);

    bool        isDeclared(const QString &id) const;
    QStringList pointIds() const;
    QVariantList points() const;          ///< [{id, title, consumed}]

    /// Why a contribution would be refused; empty string means "acceptable".
    QString validate(const QString &pointId, const QVariantMap &data) const;

    /// Add a contribution without a disposal group: the registry owns the handle.
    /// @return nullptr on refusal; @p error carries a ready-to-show reason
    PluginDisposable *add(const QString &pluginId, const QString &pointId,
                          const QVariantMap &data, QString *error = nullptr);

    /// Add a contribution whose handle is tracked by @p group (normal plugin path:
    /// unload disposes the whole group).
    PluginDisposable *add(const QString &pluginId, const QString &pointId,
                          const QVariantMap &data, DisposalGroup *group,
                          QString *error);

    /// Contributions of one point, each map carrying `pluginId` and `token`.
    QVariantList contributions(const QString &pointId) const;

    QVariantList contributionsOf(const QString &pluginId) const;

    /// Drop every contribution of one plugin. Called by the loader on unload even if
    /// the plugin never disposed its handles.
    /// @return how many were removed
    int removeAllOf(const QString &pluginId);

    /// Drop one plugin's contributions to one point (`host.clearExtensions`).
    /// @return how many were removed
    int removeContributionsOf(const QString &pluginId, const QString &pointId);

    /// Drop a single contribution by token.
    bool removeContribution(int token);

    int  count() const { return m_entries.size(); }
    void clear();

signals:
    /// Emitted once per contribution added or removed.
    void changed(const QString &pointId);

private:
    struct Entry
    {
        int                     token = 0;
        QString                 pluginId;
        QString                 pointId;
        QVariantMap             data;
        QPointer<PluginDisposable> handle;
    };

    PluginDisposable *addEntry(const QString &pluginId, const QString &pointId,
                               const QVariantMap &data, DisposalGroup *group,
                               QString *error);

    void removeToken(int token);

    int                          m_nextToken = 1;
    QHash<QString, ExtensionPoint> m_points;
    QList<Entry>                 m_entries;
};
