#include "database/NotesDatabase.h"
#include "core/Logger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDir>
#include <QFileInfo>
#include <QVariantMap>

NotesDatabase::NotesDatabase(QObject *parent)
    : QObject(parent)
    , m_connectionName(QStringLiteral("notes"))
{
}

NotesDatabase::~NotesDatabase()
{
    // 顺序很重要：先销毁 QSqlDatabase（它持有连接引用），再 removeDatabase
    // （否则 Qt 警告 "connection 'notes' is still in use"）
    if (m_db) {
        if (m_db->isOpen())
            m_db->close();
        delete m_db;
        m_db = nullptr;
    }

    if (!m_connectionName.isEmpty())
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool NotesDatabase::open(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE", m_connectionName));
    m_db->setDatabaseName(path);

    if (!m_db->open()) {
        Logger::get().error("NotesDatabase: cannot open {} — {}",
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
    Logger::get().info("NotesDatabase: opened {}", path.toStdString());
    return true;
}

bool NotesDatabase::isOpen() const
{
    return m_db && m_db->isOpen();
}

void NotesDatabase::createSchema()
{
    QSqlQuery q(*m_db);
    q.exec(
        "CREATE TABLE IF NOT EXISTS notes ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title      TEXT    NOT NULL DEFAULT '',"
        "  content    TEXT    NOT NULL DEFAULT '',"
        "  created_at TEXT    NOT NULL DEFAULT (datetime('now','localtime')),"
        "  updated_at TEXT    NOT NULL DEFAULT (datetime('now','localtime'))"
        ")"
    );
    if (q.lastError().isValid())
        Logger::get().warn("NotesDatabase: create notes — {}",
                           q.lastError().text().toStdString());
}

int NotesDatabase::addNote(const QString &title, const QString &content)
{
    if (!isOpen()) return -1;

    QSqlQuery q(*m_db);
    q.prepare("INSERT INTO notes (title, content) VALUES (:t, :c)");
    q.bindValue(":t", title);
    q.bindValue(":c", content);

    if (!q.exec()) {
        Logger::get().warn("NotesDatabase: addNote failed — {}",
                           q.lastError().text().toStdString());
        return -1;
    }
    return q.lastInsertId().toInt();
}

void NotesDatabase::updateNote(int id, const QString &title, const QString &content)
{
    if (!isOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare(
        "UPDATE notes SET title = :t, content = :c, "
        "  updated_at = datetime('now','localtime') "
        "WHERE id = :id"
    );
    q.bindValue(":t", title);
    q.bindValue(":c", content);
    q.bindValue(":id", id);

    if (!q.exec())
        Logger::get().warn("NotesDatabase: updateNote failed — {}",
                           q.lastError().text().toStdString());
}

void NotesDatabase::deleteNote(int id)
{
    if (!isOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare("DELETE FROM notes WHERE id = :id");
    q.bindValue(":id", id);

    if (!q.exec())
        Logger::get().warn("NotesDatabase: deleteNote failed — {}",
                           q.lastError().text().toStdString());
}

QVariantList NotesDatabase::listNotes()
{
    QVariantList result;
    if (!isOpen()) return result;

    QSqlQuery q(*m_db);
    q.exec(
        "SELECT id, title, content, created_at, updated_at FROM notes "
        "ORDER BY updated_at DESC"
    );
    if (q.lastError().isValid())
        Logger::get().warn("NotesDatabase: listNotes failed — {}",
                           q.lastError().text().toStdString());

    while (q.next()) {
        QVariantMap item;
        item["id"]        = q.value(0).toInt();
        item["title"]     = q.value(1).toString();
        item["content"]   = q.value(2).toString();
        item["createdAt"] = q.value(3).toString();
        item["updatedAt"] = q.value(4).toString();
        result.append(item);
    }
    return result;
}
