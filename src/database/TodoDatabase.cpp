#include "database/TodoDatabase.h"
#include "core/Logger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDir>
#include <QFileInfo>
#include <QVariantMap>

TodoDatabase::TodoDatabase(QObject *parent)
    : QObject(parent)
    , m_connectionName(QStringLiteral("todo"))
{
}

TodoDatabase::~TodoDatabase()
{
    // 顺序很重要：先销毁 QSqlDatabase（它持有连接引用），再 removeDatabase
    // （否则 Qt 警告 "connection 'todo' is still in use"）
    if (m_db) {
        if (m_db->isOpen())
            m_db->close();
        delete m_db;
        m_db = nullptr;
    }

    if (!m_connectionName.isEmpty())
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool TodoDatabase::open(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE", m_connectionName));
    m_db->setDatabaseName(path);

    if (!m_db->open()) {
        Logger::get().error("TodoDatabase: cannot open {} — {}",
                            path.toStdString(),
                            m_db->lastError().text().toStdString());
        return false;
    }

    {
        QSqlQuery q(*m_db);
        q.exec("PRAGMA journal_mode=WAL");
        q.exec("PRAGMA synchronous=NORMAL");
    }

    createSchema();
    Logger::get().info("TodoDatabase: opened {}", path.toStdString());
    return true;
}

bool TodoDatabase::isOpen() const
{
    return m_db && m_db->isOpen();
}

void TodoDatabase::createSchema()
{
    QSqlQuery q(*m_db);
    q.exec(
        "CREATE TABLE IF NOT EXISTS todos ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  content      TEXT    NOT NULL,"
        "  done         INTEGER NOT NULL DEFAULT 0,"
        "  created_at   TEXT    NOT NULL DEFAULT (datetime('now','localtime')),"
        "  completed_at TEXT"
        ")"
    );
    if (q.lastError().isValid())
        Logger::get().warn("TodoDatabase: create todos — {}",
                           q.lastError().text().toStdString());
}

int TodoDatabase::addTodo(const QString &content)
{
    if (!isOpen() || content.trimmed().isEmpty())
        return -1;

    QSqlQuery q(*m_db);
    q.prepare("INSERT INTO todos (content) VALUES (:c)");
    q.bindValue(":c", content.trimmed());

    if (!q.exec()) {
        Logger::get().warn("TodoDatabase: addTodo failed — {}",
                           q.lastError().text().toStdString());
        return -1;
    }
    return q.lastInsertId().toInt();
}

void TodoDatabase::toggleDone(int id)
{
    if (!isOpen()) return;

    // SQLite 单条 UPDATE 的 SET 表达式均基于旧行值：
    // 旧 done=0 → 新 done=1 且写 completed_at；旧 done=1 → 新 done=0 且清 completed_at。
    QSqlQuery q(*m_db);
    q.prepare(
        "UPDATE todos SET "
        "  done = 1 - done,"
        "  completed_at = CASE WHEN done = 0 THEN datetime('now','localtime') ELSE NULL END "
        "WHERE id = :id"
    );
    q.bindValue(":id", id);

    if (!q.exec())
        Logger::get().warn("TodoDatabase: toggleDone failed — {}",
                           q.lastError().text().toStdString());
}

void TodoDatabase::deleteTodo(int id)
{
    if (!isOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare("DELETE FROM todos WHERE id = :id");
    q.bindValue(":id", id);

    if (!q.exec())
        Logger::get().warn("TodoDatabase: deleteTodo failed — {}",
                           q.lastError().text().toStdString());
}

QVariantList TodoDatabase::listTodos()
{
    QVariantList result;
    if (!isOpen()) return result;

    QSqlQuery q(*m_db);
    q.exec(
        "SELECT id, content, done, created_at, completed_at FROM todos "
        "ORDER BY done ASC, "
        "  CASE WHEN done = 0 THEN created_at END ASC, "
        "  CASE WHEN done = 1 THEN completed_at END DESC"
    );
    if (q.lastError().isValid())
        Logger::get().warn("TodoDatabase: listTodos failed — {}",
                           q.lastError().text().toStdString());

    while (q.next()) {
        QVariantMap item;
        item["id"]          = q.value(0).toInt();
        item["content"]     = q.value(1).toString();
        item["done"]        = q.value(2).toInt() != 0;
        item["createdAt"]   = q.value(3).toString();
        item["completedAt"] = q.value(4).toString();
        result.append(item);
    }
    return result;
}
