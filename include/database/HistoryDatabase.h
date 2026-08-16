#pragma once

#include <QObject>
#include <QDateTime>
#include <QStringList>
#include <QVariantList>

class QSqlDatabase;

/// @brief SQLite-backed history database for the Assistant app.
///
/// Manages three tables:
///   search_history   — every search query submitted by the user
///   command_history  — every command/app executed, with launch counts
///   app_launch_history — per-app launch tracking for priority boosting
///
/// Uses Qt6::Sql (QSqlDatabase / QSqlQuery).  The connection is named
/// "history" so it does not interfere with other database users.
class HistoryDatabase : public QObject
{
    Q_OBJECT

public:
    explicit HistoryDatabase(QObject *parent = nullptr);
    ~HistoryDatabase() override;

    /// Open (or create) the database at @p path.
    /// Returns true on success.
    bool open(const QString &path);

    bool isOpen() const;

    // ---- search history ----

    /// Record a search query with its result count.
    void recordSearch(const QString &query, int resultCount = 0);

    /// Return recent searches whose query starts with @p prefix.
    /// Each element is a QVariantMap with keys: id, query, timestamp, type="history".
    QVariantList recentSearches(const QString &prefix, int limit = 5);

    // ---- command / app launch history ----

    /// Record that a command with the given id was executed.
    /// Increments execution_count and updates last_executed.
    void recordCommandExecution(const QString &commandId, const QString &displayName);

    /// Record an app launch.  Returns the new launch_count.
    int recordAppLaunch(const QString &appName, const QString &appPath);

    /// Return most-frequently-launched apps whose name matches @p prefix.
    QVariantList frequentApps(const QString &prefix, int limit = 5);

    /// Convenience: raw launch count for a given app path.
    int launchCount(const QString &appPath) const;

    // ---- maintenance ----

    /// Delete entries older than @p olderThanDays.
    void cleanup(int olderThanDays = 90);

    /// Total number of rows across all history tables.
    int entryCount() const;

signals:
    void databaseError(const QString &message);

private:
    void createSchema();

    QSqlDatabase *m_db = nullptr;
    QString m_connectionName;
};
