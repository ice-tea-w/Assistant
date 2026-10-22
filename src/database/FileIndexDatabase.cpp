#include "database/FileIndexDatabase.h"
#include "database/DatabaseMigrator.h"
#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

/// 一条 UPSERT 覆盖 added 与 changed 两种情况。
/// 注意 `frecency` **不在** DO UPDATE 里 —— 它是用户行为的累积，重扫不该清零。
const char *kUpsertSql =
    "INSERT INTO files (path, name, name_lower, dir_id, ext, size, mtime) "
    "VALUES (:path, :name, :name_lower, :dir_id, :ext, :size, :mtime) "
    "ON CONFLICT(path) DO UPDATE SET "
    "  name = excluded.name, name_lower = excluded.name_lower, dir_id = excluded.dir_id,"
    "  ext = excluded.ext, size = excluded.size, mtime = excluded.mtime";

bool execSql(QSqlDatabase &db, const QString &sql, QString *error)
{
    QSqlQuery q(db);
    if (q.exec(sql))
        return true;
    if (error)
        *error = q.lastError().text();
    Logger::get().error("FileIndexDatabase: SQL 失败 — {} — {}",
                        sql.toStdString(), q.lastError().text().toStdString());
    return false;
}

/// v1 = §3.5 的三张表 + 两个索引
bool createSchemaV1(QSqlDatabase &db)
{
    QString err;

    if (!execSql(db,
            QStringLiteral("CREATE TABLE IF NOT EXISTS dirs ("
                           "  id   INTEGER PRIMARY KEY,"
                           "  path TEXT NOT NULL UNIQUE"
                           ")"), &err))
        return false;

    // WITHOUT ROWID：path 就是主键，省掉一层 rowid 间接寻址
    if (!execSql(db,
            QStringLiteral("CREATE TABLE IF NOT EXISTS files ("
                           "  path       TEXT PRIMARY KEY,"
                           "  name       TEXT NOT NULL,"
                           "  name_lower TEXT NOT NULL,"
                           "  dir_id     INTEGER NOT NULL REFERENCES dirs(id),"
                           "  ext        TEXT,"
                           "  size       INTEGER NOT NULL,"
                           "  mtime      INTEGER NOT NULL,"
                           "  frecency   REAL NOT NULL DEFAULT 0"
                           ") WITHOUT ROWID"), &err))
        return false;

    if (!execSql(db, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_dir ON files(dir_id)"), &err))
        return false;
    if (!execSql(db, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_free ON files(frecency DESC)"), &err))
        return false;

    if (!execSql(db,
            QStringLiteral("CREATE TABLE IF NOT EXISTS roots ("
                           "  path      TEXT PRIMARY KEY,"
                           "  enabled   INTEGER NOT NULL DEFAULT 1,"
                           "  last_scan INTEGER"
                           ")"), &err))
        return false;

    return true;
}

/// v1 → v2：治理字段增补 + 离线去向反查要用的索引（§3.5 / §3.6）
bool migrateToV2(QSqlDatabase &db)
{
    QString err;

    // 只加列，绝不 DROP + 重建
    if (!execSql(db, QStringLiteral("ALTER TABLE files ADD COLUMN cat_mask  INTEGER NOT NULL DEFAULT 0"), &err))
        return false;
    if (!execSql(db, QStringLiteral("ALTER TABLE files ADD COLUMN flags     INTEGER NOT NULL DEFAULT 0"), &err))
        return false;
    if (!execSql(db, QStringLiteral("ALTER TABLE files ADD COLUMN root_id   INTEGER NOT NULL DEFAULT 0"), &err))
        return false;
    if (!execSql(db, QStringLiteral("ALTER TABLE files ADD COLUMN origin_id INTEGER NOT NULL DEFAULT 0"), &err))
        return false;

    // 「离线去向反查」：按 (size, mtime) 把候选缩到个位数（治理文档 §2.6 第六步）
    if (!execSql(db, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_sizemtime ON files(size, mtime)"), &err))
        return false;

    return true;
}

} // namespace

FileIndexDatabase::FileIndexDatabase(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

FileIndexDatabase::~FileIndexDatabase()
{
    close();
}

bool FileIndexDatabase::open(const QString &dbPath)
{
    close();
    m_path = dbPath;

    QDir().mkpath(QFileInfo(dbPath).absolutePath());

    m_db = new QSqlDatabase(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName));
    m_db->setDatabaseName(dbPath);

    if (!m_db->open()) {
        m_lastError = m_db->lastError().text();
        Logger::get().error("FileIndexDatabase: 打不开 {} — {}",
                            dbPath.toStdString(), m_lastError.toStdString());
        close();
        return false;
    }

    // PRAGMA 必须在事务外
    {
        QSqlQuery q(*m_db);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        q.exec(QStringLiteral("PRAGMA cache_size=-8000"));
    }

    if (!ensureSchema()) {
        close();
        return false;
    }

    preloadDirIds();

    Logger::get().info("FileIndexDatabase: opened {} ({} rows)", dbPath.toStdString(), rowCount());
    return true;
}

void FileIndexDatabase::close()
{
    m_dirIds.clear();

    // 顺序很重要：先销毁 QSqlDatabase（持有连接引用）再 removeDatabase，
    // 否则 Qt 警告 "connection … is still in use"（Phase 3 四个库踩过）
    if (m_db) {
        if (m_db->isOpen())
            m_db->close();
        delete m_db;
        m_db = nullptr;
    }

    if (!m_connectionName.isEmpty() && QSqlDatabase::contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool FileIndexDatabase::isOpen() const
{
    return m_db && m_db->isOpen();
}

QString FileIndexDatabase::lastError() const
{
    return m_lastError;
}

bool FileIndexDatabase::ensureSchema()
{
    DatabaseMigrator migrator(QStringLiteral("file_index"), {
        { 1, QStringLiteral("建 dirs/files/roots 与两个索引"), &createSchemaV1 },
        { 2, QStringLiteral("加 cat_mask/flags/root_id/origin_id + idx_files_sizemtime"), &migrateToV2 },
    });

    if (migrator.run(*m_db))
        return true;

    m_lastError = QStringLiteral("schema 迁移失败");
    return false;
}

QVector<FileIndexDatabase::FileRow> FileIndexDatabase::loadAll() const
{
    QVector<FileRow> rows;
    if (!isOpen())
        return rows;

    QSqlQuery q(*m_db);
    if (!q.exec(QStringLiteral("SELECT path, name, size, mtime FROM files"))) {
        Logger::get().warn("FileIndexDatabase: loadAll 失败 — {}",
                           q.lastError().text().toStdString());
        return rows;
    }

    while (q.next()) {
        FileRow row;
        row.path  = q.value(0).toString();
        row.name  = q.value(1).toString();
        row.size  = q.value(2).toLongLong();
        row.mtime = q.value(3).toLongLong();
        rows.append(std::move(row));
    }
    return rows;
}

int FileIndexDatabase::rowCount() const
{
    if (!isOpen())
        return 0;

    QSqlQuery q(*m_db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM files")) || !q.next())
        return 0;
    return q.value(0).toInt();
}

void FileIndexDatabase::preloadDirIds()
{
    if (!isOpen())
        return;

    QSqlQuery q(*m_db);
    if (!q.exec(QStringLiteral("SELECT id, path FROM dirs"))) {
        Logger::get().warn("FileIndexDatabase: 预载 dirs 失败 — {}",
                           q.lastError().text().toStdString());
        return;
    }
    while (q.next())
        m_dirIds.insert(q.value(1).toString(), q.value(0).toLongLong());
}

qint64 FileIndexDatabase::dirId(const QString &dirPath)
{
    auto it = m_dirIds.constFind(dirPath);
    if (it != m_dirIds.constEnd())
        return it.value();

    QSqlQuery ins(*m_db);
    ins.prepare(QStringLiteral("INSERT OR IGNORE INTO dirs(path) VALUES (:p)"));
    ins.bindValue(QStringLiteral(":p"), dirPath);
    if (!ins.exec()) {
        m_lastError = ins.lastError().text();
        return -1;
    }

    QSqlQuery sel(*m_db);
    sel.prepare(QStringLiteral("SELECT id FROM dirs WHERE path = :p"));
    sel.bindValue(QStringLiteral(":p"), dirPath);
    if (!sel.exec() || !sel.next()) {
        m_lastError = sel.lastError().text();
        return -1;
    }

    const qint64 id = sel.value(0).toLongLong();
    m_dirIds.insert(dirPath, id);
    return id;
}

bool FileIndexDatabase::applyDiff(const Diff &diff)
{
    if (!isOpen())
        return false;
    if (diff.empty())
        return true;

    QSqlQuery upsert(*m_db);
    if (!upsert.prepare(QString::fromLatin1(kUpsertSql))) {
        m_lastError = upsert.lastError().text();
        return false;
    }

    QSqlQuery del(*m_db);
    if (!del.prepare(QStringLiteral("DELETE FROM files WHERE path = :path"))) {
        m_lastError = del.lastError().text();
        return false;
    }

    int inBatch = 0;
    auto beginBatch = [&]() -> bool {
        if (m_db->transaction()) {
            inBatch = 0;
            return true;
        }
        m_lastError = m_db->lastError().text();
        return false;
    };

    auto flushBatch = [&]() -> bool {
        if (!m_db->commit()) {
            m_lastError = m_db->lastError().text();
            m_db->rollback();
            return false;
        }
        return beginBatch();
    };

    auto writeRow = [&](const FileRow &row) -> bool {
        const int slash = row.path.lastIndexOf(QLatin1Char('/'));
        const QString dirPath = slash > 0 ? row.path.left(slash) : QString();

        const qint64 did = dirId(dirPath);
        if (did < 0)
            return false;

        const int dot = row.name.lastIndexOf(QLatin1Char('.'));
        const QString ext = (dot > 0 && dot + 1 < row.name.size())
                                ? row.name.mid(dot + 1).toLower()
                                : QString();

        upsert.bindValue(QStringLiteral(":path"),       row.path);
        upsert.bindValue(QStringLiteral(":name"),       row.name);
        upsert.bindValue(QStringLiteral(":name_lower"), row.name.toLower());
        upsert.bindValue(QStringLiteral(":dir_id"),     did);
        upsert.bindValue(QStringLiteral(":ext"),        ext);
        upsert.bindValue(QStringLiteral(":size"),       row.size);
        upsert.bindValue(QStringLiteral(":mtime"),      row.mtime);

        if (!upsert.exec()) {
            m_lastError = upsert.lastError().text();
            return false;
        }
        return true;
    };

    if (!beginBatch())
        return false;

    // added 与 changed 走同一条 UPSERT（changed 就是"整行覆盖一次"）
    for (const FileRow &row : diff.added) {
        if (!writeRow(row)) { m_db->rollback(); return false; }
        if (++inBatch >= 1000 && !flushBatch()) return false;
    }
    for (const FileRow &row : diff.changed) {
        if (!writeRow(row)) { m_db->rollback(); return false; }
        if (++inBatch >= 1000 && !flushBatch()) return false;
    }
    for (const QString &path : diff.removed) {
        del.bindValue(QStringLiteral(":path"), path);
        if (!del.exec()) {
            m_lastError = del.lastError().text();
            m_db->rollback();
            return false;
        }
        if (++inBatch >= 1000 && !flushBatch()) return false;
    }

    if (!m_db->commit()) {
        m_lastError = m_db->lastError().text();
        m_db->rollback();
        return false;
    }
    return true;
}

bool FileIndexDatabase::touchRoot(const QString &rootPath, qint64 epochSec)
{
    if (!isOpen())
        return false;

    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO roots (path, enabled, last_scan) VALUES (:p, 1, :t) "
        "ON CONFLICT(path) DO UPDATE SET last_scan = excluded.last_scan"));
    q.bindValue(QStringLiteral(":p"), rootPath);
    q.bindValue(QStringLiteral(":t"), epochSec);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool FileIndexDatabase::removeDatabaseFile(const QString &dbPath)
{
    // 连同 WAL 边车文件一起删，否则重建后可能读到旧 WAL
    const QStringList sidecars = { QString(), QStringLiteral("-wal"), QStringLiteral("-shm") };
    bool allOk = true;
    for (const QString &suffix : sidecars) {
        const QString f = dbPath + suffix;
        if (QFile::exists(f) && !QFile::remove(f)) {
            Logger::get().warn("FileIndexDatabase: 删不掉 {}（可能被占用）", f.toStdString());
            allOk = false;
        }
    }
    return allOk;
}

FileIndexDatabase::Diff FileIndexDatabase::diff(const QVector<FileRow> &diskRows,
                                                const QVector<FileRow> &dbRows)
{
    Diff out;

    QHash<QString, int> dbIndex;
    dbIndex.reserve(dbRows.size());
    for (int i = 0; i < dbRows.size(); ++i)
        dbIndex.insert(dbRows.at(i).path, i);

    QVector<char> seen(dbRows.size(), 0);

    for (const FileRow &disk : diskRows) {
        const auto it = dbIndex.constFind(disk.path);
        if (it == dbIndex.constEnd()) {
            out.added.append(disk);
            continue;
        }

        seen[it.value()] = 1;
        const FileRow &db = dbRows.at(it.value());
        if (db.size == disk.size && db.mtime == disk.mtime)
            ++out.unchanged;
        else
            out.changed.append(disk);
    }

    for (int i = 0; i < dbRows.size(); ++i) {
        if (!seen.at(i))
            out.removed.append(dbRows.at(i).path);
    }

    return out;
}
