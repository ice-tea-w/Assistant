#include "database/ClipboardDatabase.h"
#include "core/Logger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QVariantMap>

ClipboardDatabase::ClipboardDatabase(QObject *parent)
    : QObject(parent)
    , m_connectionName(QStringLiteral("clipboard"))
{
}

ClipboardDatabase::~ClipboardDatabase()
{
    // 顺序很重要：先销毁 QSqlDatabase（它持有连接引用），再 removeDatabase
    // （否则 Qt 警告 "connection 'clipboard' is still in use"）
    if (m_db) {
        if (m_db->isOpen())
            m_db->close();
        delete m_db;
        m_db = nullptr;
    }

    if (!m_connectionName.isEmpty())
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool ClipboardDatabase::open(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE", m_connectionName));
    m_db->setDatabaseName(path);

    if (!m_db->open()) {
        Logger::get().error("ClipboardDatabase: cannot open {} — {}",
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
    Logger::get().info("ClipboardDatabase: opened {}", path.toStdString());
    return true;
}

bool ClipboardDatabase::isOpen() const
{
    return m_db && m_db->isOpen();
}

void ClipboardDatabase::createSchema()
{
    QSqlQuery q(*m_db);
    q.exec(
        "CREATE TABLE IF NOT EXISTS clipboard_history ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  content    TEXT    NOT NULL,"
        "  mime_type  TEXT    NOT NULL DEFAULT 'text/plain',"
        "  created_at TEXT    NOT NULL DEFAULT (datetime('now','localtime'))"
        ")"
    );
    if (q.lastError().isValid())
        Logger::get().warn("ClipboardDatabase: create clipboard_history — {}",
                           q.lastError().text().toStdString());
}

int ClipboardDatabase::addClip(const QString &content, const QString &mimeType)
{
    if (!isOpen() || content.isEmpty())
        return -1;

    // 连续相同内容去重：与最近一条相同则跳过（避免复制同一内容刷屏）
    QSqlQuery last(*m_db);
    last.exec("SELECT content FROM clipboard_history ORDER BY id DESC LIMIT 1");
    if (last.next() && last.value(0).toString() == content)
        return -1;

    // 内容已存在于历史其他位置 → 删除旧条目，实现「重新复制即置顶」
    {
        QSqlQuery del(*m_db);
        del.prepare("DELETE FROM clipboard_history WHERE content = :c");
        del.bindValue(":c", content);
        del.exec();
    }

    QSqlQuery q(*m_db);
    q.prepare("INSERT INTO clipboard_history (content, mime_type) VALUES (:c, :m)");
    q.bindValue(":c", content);
    q.bindValue(":m", mimeType.isEmpty() ? QStringLiteral("text/plain") : mimeType);

    if (!q.exec()) {
        Logger::get().warn("ClipboardDatabase: addClip failed — {}",
                           q.lastError().text().toStdString());
        return -1;
    }
    return q.lastInsertId().toInt();
}

void ClipboardDatabase::deleteClip(int id)
{
    if (!isOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare("DELETE FROM clipboard_history WHERE id = :id");
    q.bindValue(":id", id);

    if (!q.exec())
        Logger::get().warn("ClipboardDatabase: deleteClip failed — {}",
                           q.lastError().text().toStdString());
}

void ClipboardDatabase::clearAll()
{
    if (!isOpen()) return;

    QSqlQuery q(*m_db);
    if (!q.exec("DELETE FROM clipboard_history"))
        Logger::get().warn("ClipboardDatabase: clearAll failed — {}",
                           q.lastError().text().toStdString());
}

QVariantList ClipboardDatabase::listClips(int limit)
{
    QVariantList result;
    if (!isOpen()) return result;

    QSqlQuery q(*m_db);
    q.prepare(
        "SELECT id, content, mime_type, created_at FROM clipboard_history "
        "ORDER BY id DESC LIMIT :lim"
    );
    q.bindValue(":lim", limit);

    if (!q.exec()) {
        Logger::get().warn("ClipboardDatabase: listClips failed — {}",
                           q.lastError().text().toStdString());
        return result;
    }

    while (q.next()) {
        QVariantMap item;
        item["id"]        = q.value(0).toInt();
        item["content"]   = q.value(1).toString();
        item["mimeType"]  = q.value(2).toString();

        // created_at 由 SQLite 的 datetime('now','localtime') 写成
        // "YYYY-MM-DD HH:MM:SS"（本地时间）。这里解析成本地 QDateTime 再转 epoch，
        // 供 QML 计算"刚刚 / N 分钟前"；不要用 strftime('%s', ...) —— 那会把
        // 本地时间当 UTC 解释，多出时区偏移。
        const QString createdAt = q.value(3).toString();
        item["createdAt"] = createdAt;

        const QDateTime dt = QDateTime::fromString(
            createdAt, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        item["createdAtEpoch"] = dt.isValid() ? dt.toSecsSinceEpoch() : 0;

        result.append(item);
    }
    return result;
}

QString ClipboardDatabase::clipContent(int id) const
{
    if (!m_db || !m_db->isOpen()) return {};

    QSqlQuery q(*const_cast<QSqlDatabase *>(m_db));
    q.prepare("SELECT content FROM clipboard_history WHERE id = :id");
    q.bindValue(":id", id);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}
