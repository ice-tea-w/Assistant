#include "database/HistoryDatabase.h"
#include "core/Logger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QVariantMap>
#include <QDateTime>

// ---- construction / destruction ----

HistoryDatabase::HistoryDatabase(QObject *parent)
    : QObject(parent)
    , m_connectionName(QStringLiteral("history"))
{
}

HistoryDatabase::~HistoryDatabase()
{
    if (m_db && m_db->isOpen())
        m_db->close();

    if (!m_connectionName.isEmpty())
        QSqlDatabase::removeDatabase(m_connectionName);
}

// ---- open ----

bool HistoryDatabase::open(const QString &path)
{
    // Ensure parent directory exists
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE", m_connectionName));
    m_db->setDatabaseName(path);

    if (!m_db->open()) {
        const QString err = m_db->lastError().text();
        Logger::get().error("HistoryDatabase: cannot open {} — {}", path.toStdString(), err.toStdString());
        emit databaseError(err);
        return false;
    }

    // Enable WAL mode for concurrent reads + writes (lightweight)
    {
        QSqlQuery q(*m_db);
        q.exec("PRAGMA journal_mode=WAL");
        q.exec("PRAGMA synchronous=NORMAL");
    }

    createSchema();

    Logger::get().info("HistoryDatabase: opened {} ({} entries)",
                       path.toStdString(), entryCount());
    return true;
}

bool HistoryDatabase::isOpen() const
{
    return m_db && m_db->isOpen();
}

// ---- schema ----

void HistoryDatabase::createSchema()
{
    QSqlQuery q(*m_db);

    q.exec(
        "CREATE TABLE IF NOT EXISTS search_history ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  query         TEXT    NOT NULL,"
        "  result_count  INTEGER NOT NULL DEFAULT 0,"
        "  created_at    TEXT    NOT NULL DEFAULT (datetime('now','localtime'))"
        ")"
    );
    if (q.lastError().isValid())
        Logger::get().warn("HistoryDatabase: create search_history — {}", q.lastError().text().toStdString());

    q.exec(
        "CREATE TABLE IF NOT EXISTS command_history ("
        "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  command_id      TEXT    NOT NULL UNIQUE,"
        "  display_name    TEXT    NOT NULL,"
        "  execution_count INTEGER NOT NULL DEFAULT 1,"
        "  last_executed   TEXT    NOT NULL DEFAULT (datetime('now','localtime'))"
        ")"
    );
    if (q.lastError().isValid())
        Logger::get().warn("HistoryDatabase: create command_history — {}", q.lastError().text().toStdString());

    q.exec(
        "CREATE TABLE IF NOT EXISTS app_launch_history ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  app_name      TEXT    NOT NULL,"
        "  app_path      TEXT    NOT NULL UNIQUE,"
        "  launch_count  INTEGER NOT NULL DEFAULT 1,"
        "  last_launched TEXT    NOT NULL DEFAULT (datetime('now','localtime'))"
        ")"
    );
    if (q.lastError().isValid())
        Logger::get().warn("HistoryDatabase: create app_launch_history — {}", q.lastError().text().toStdString());

    // Indexes
    q.exec("CREATE INDEX IF NOT EXISTS idx_search_query ON search_history(query)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_search_created ON search_history(created_at)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_command_count ON command_history(execution_count DESC)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_app_launch_count ON app_launch_history(launch_count DESC)");
}

// ---- search history ----

void HistoryDatabase::recordSearch(const QString &query, int resultCount)
{
    if (!isOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare("INSERT INTO search_history (query, result_count) VALUES (:q, :rc)");
    q.bindValue(":q",  query);
    q.bindValue(":rc", resultCount);

    if (!q.exec())
        Logger::get().warn("HistoryDatabase: recordSearch failed — {}",
                           q.lastError().text().toStdString());
}

QVariantList HistoryDatabase::recentSearches(const QString &prefix, int limit)
{
    QVariantList results;
    if (!isOpen() || prefix.isEmpty()) return results;

    QSqlQuery q(*m_db);
    q.prepare(
        "SELECT DISTINCT query FROM search_history "
        " WHERE query LIKE :prefix "
        " ORDER BY MAX(created_at) DESC "
        " LIMIT :lim"
    );
    q.bindValue(":prefix", prefix + '%');
    q.bindValue(":lim",    limit);

    if (!q.exec()) {
        Logger::get().warn("HistoryDatabase: recentSearches failed — {}",
                           q.lastError().text().toStdString());
        return results;
    }

    while (q.next()) {
        QVariantMap item;
        item["typeName"] = QStringLiteral("history");
        item["title"]    = q.value(0).toString();
        results.append(item);
    }
    return results;
}

// ---- command / app launch history ----

void HistoryDatabase::recordCommandExecution(const QString &commandId, const QString &displayName)
{
    if (!isOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare(
        "INSERT INTO command_history (command_id, display_name) VALUES (:id, :dn) "
        "ON CONFLICT(command_id) DO UPDATE SET "
        "  execution_count = execution_count + 1,"
        "  last_executed   = datetime('now','localtime')"
    );
    q.bindValue(":id", commandId);
    q.bindValue(":dn", displayName);

    if (!q.exec())
        Logger::get().warn("HistoryDatabase: recordCommandExecution failed — {}",
                           q.lastError().text().toStdString());
}

int HistoryDatabase::recordAppLaunch(const QString &appName, const QString &appPath)
{
    if (!isOpen()) return 0;

    QSqlQuery q(*m_db);
    q.prepare(
        "INSERT INTO app_launch_history (app_name, app_path) VALUES (:name, :path) "
        "ON CONFLICT(app_path) DO UPDATE SET "
        "  launch_count  = launch_count + 1,"
        "  last_launched = datetime('now','localtime')"
    );
    q.bindValue(":name", appName);
    q.bindValue(":path", appPath);

    if (!q.exec()) {
        Logger::get().warn("HistoryDatabase: recordAppLaunch failed — {}",
                           q.lastError().text().toStdString());
        return 0;
    }

    // Read back the current count
    QSqlQuery r(*m_db);
    r.prepare("SELECT launch_count FROM app_launch_history WHERE app_path = :path");
    r.bindValue(":path", appPath);
    if (r.exec() && r.next())
        return r.value(0).toInt();

    return 1;
}

QVariantList HistoryDatabase::frequentApps(const QString &prefix, int limit)
{
    QVariantList results;
    if (!isOpen()) return results;

    QSqlQuery q(*m_db);
    q.prepare(
        "SELECT app_name, app_path, launch_count FROM app_launch_history "
        " WHERE app_name LIKE :prefix "
        " ORDER BY launch_count DESC "
        " LIMIT :lim"
    );
    q.bindValue(":prefix", prefix + '%');
    q.bindValue(":lim",    limit);

    if (!q.exec()) {
        Logger::get().warn("HistoryDatabase: frequentApps failed — {}",
                           q.lastError().text().toStdString());
        return results;
    }

    while (q.next()) {
        QVariantMap item;
        item["typeName"]  = QStringLiteral("history");
        item["title"]     = q.value(0).toString();
        item["subtitle"]  = QStringLiteral("Launched %1 times").arg(q.value(2).toInt());
        results.append(item);
    }
    return results;
}

int HistoryDatabase::launchCount(const QString &appPath) const
{
    if (!m_db || !m_db->isOpen()) return 0;

    QSqlQuery q(*const_cast<QSqlDatabase *>(m_db));
    q.prepare("SELECT launch_count FROM app_launch_history WHERE app_path = :path");
    q.bindValue(":path", appPath);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

// ---- maintenance ----

void HistoryDatabase::cleanup(int olderThanDays)
{
    if (!isOpen()) return;

    QSqlQuery q(*m_db);
    QString cond = QString("created_at < datetime('now','localtime','-%1 days')").arg(olderThanDays);
    q.exec("DELETE FROM search_history WHERE " + cond);
    q.exec("DELETE FROM command_history WHERE last_executed < datetime('now','localtime','-90 days')");

    Logger::get().info("HistoryDatabase: cleanup complete (entries: {})", entryCount());
}

int HistoryDatabase::entryCount() const
{
    if (!m_db || !m_db->isOpen()) return 0;

    int total = 0;
    auto cnt = [&](const QString &table) {
        QSqlQuery q(*const_cast<QSqlDatabase *>(m_db));
        if (q.exec("SELECT COUNT(*) FROM " + table) && q.next())
            total += q.value(0).toInt();
    };
    cnt("search_history");
    cnt("command_history");
    cnt("app_launch_history");
    return total;
}
