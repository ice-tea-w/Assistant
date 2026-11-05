#include "database/SyncDatabase.h"
#include "core/Logger.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

const char *kSchema[] = {
    R"(CREATE TABLE IF NOT EXISTS sync_state (
        rel_path   TEXT PRIMARY KEY,
        size       INTEGER NOT NULL,
        mtime      INTEGER NOT NULL,
        hash       TEXT NOT NULL,
        synced_at  INTEGER NOT NULL,
        direction  TEXT NOT NULL
    ))",

    R"(CREATE TABLE IF NOT EXISTS conflicts (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        rel_path      TEXT NOT NULL,
        local_hash    TEXT NOT NULL,
        remote_hash   TEXT NOT NULL,
        conflict_copy TEXT NOT NULL,
        detected_at   INTEGER NOT NULL,
        resolved      INTEGER NOT NULL DEFAULT 0
    ))",

    "CREATE INDEX IF NOT EXISTS idx_conflicts_detected ON conflicts(detected_at DESC)",

    R"(CREATE TABLE IF NOT EXISTS received (
        rel_path    TEXT PRIMARY KEY,
        hash        TEXT NOT NULL,
        received_at INTEGER NOT NULL
    ))",

    R"(CREATE TABLE IF NOT EXISTS devices (
        device_id    TEXT PRIMARY KEY,
        fingerprint  TEXT NOT NULL,
        secret       BLOB NOT NULL,
        paired_at    INTEGER NOT NULL,
        last_seen_at INTEGER NOT NULL DEFAULT 0
    ))"
};

} // namespace

SyncDatabase::SyncDatabase(const QString &connectionName)
    : m_connectionName(connectionName)
{
}

bool SyncDatabase::open(const QString &filePath)
{
    if (QSqlDatabase::contains(m_connectionName)) {
        m_db = QSqlDatabase::database(m_connectionName);
    } else {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        m_db.setDatabaseName(filePath);
    }

    if (!m_db.isOpen() && !m_db.open()) {
        Logger::get().error("SyncDatabase: cannot open {} — {}",
                            filePath.toStdString(), m_db.lastError().text().toStdString());
        return false;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL;"));

    return createSchema();
}

bool SyncDatabase::createSchema()
{
    QSqlQuery q(m_db);
    for (const char *sql : kSchema) {
        if (!q.exec(QString::fromUtf8(sql))) {
            Logger::get().error("SyncDatabase: schema error — {}", q.lastError().text().toStdString());
            return false;
        }
    }
    return true;
}

bool SyncDatabase::isOpen() const
{
    return m_db.isOpen();
}

void SyncDatabase::close()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    if (QSqlDatabase::contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
}

// ---- sync_state ----

std::optional<Sync::FileState> SyncDatabase::baseline(const QString &relPath) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT size, mtime, hash FROM sync_state WHERE rel_path = ?"));
    q.addBindValue(relPath);
    if (!q.exec() || !q.next())
        return std::nullopt;

    Sync::FileState s;
    s.size  = q.value(0).toLongLong();
    s.mtime = q.value(1).toLongLong();
    s.hash  = q.value(2).toString();
    return s;
}

bool SyncDatabase::upsertBaseline(const QString &relPath, const Sync::FileState &state,
                                  qint64 syncedAt, const QString &direction)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO sync_state (rel_path, size, mtime, hash, synced_at, direction) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(rel_path) DO UPDATE SET size=excluded.size, mtime=excluded.mtime, "
        "hash=excluded.hash, synced_at=excluded.synced_at, direction=excluded.direction"));
    q.addBindValue(relPath);
    q.addBindValue(state.size);
    q.addBindValue(state.mtime);
    q.addBindValue(state.hash);
    q.addBindValue(syncedAt);
    q.addBindValue(direction);
    if (!q.exec()) {
        Logger::get().warn("SyncDatabase: upsertBaseline {} failed — {}",
                           relPath.toStdString(), q.lastError().text().toStdString());
        return false;
    }
    return true;
}

bool SyncDatabase::removeBaseline(const QString &relPath)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM sync_state WHERE rel_path = ?"));
    q.addBindValue(relPath);
    return q.exec();
}

QList<QString> SyncDatabase::allBaselinePaths() const
{
    QList<QString> out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT rel_path FROM sync_state")))
        return out;
    while (q.next())
        out.append(q.value(0).toString());
    return out;
}

// ---- conflicts ----

bool SyncDatabase::insertConflict(const ConflictRecord &rec)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO conflicts (rel_path, local_hash, remote_hash, conflict_copy, detected_at, resolved) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(rec.relPath);
    q.addBindValue(rec.localHash);
    q.addBindValue(rec.remoteHash);
    q.addBindValue(rec.conflictCopy);
    q.addBindValue(rec.detectedAt);
    q.addBindValue(rec.resolved ? 1 : 0);
    if (!q.exec()) {
        Logger::get().warn("SyncDatabase: insertConflict failed — {}", q.lastError().text().toStdString());
        return false;
    }
    return true;
}

QList<ConflictRecord> SyncDatabase::recentConflicts(int limit) const
{
    QList<ConflictRecord> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, rel_path, local_hash, remote_hash, conflict_copy, detected_at, resolved "
        "FROM conflicts ORDER BY detected_at DESC, id DESC LIMIT ?"));
    q.addBindValue(limit);
    if (!q.exec())
        return out;

    while (q.next()) {
        ConflictRecord r;
        r.id           = q.value(0).toInt();
        r.relPath      = q.value(1).toString();
        r.localHash    = q.value(2).toString();
        r.remoteHash   = q.value(3).toString();
        r.conflictCopy = q.value(4).toString();
        r.detectedAt   = q.value(5).toLongLong();
        r.resolved     = q.value(6).toInt() != 0;
        out.append(r);
    }
    return out;
}

bool SyncDatabase::markResolved(int id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE conflicts SET resolved = 1 WHERE id = ?"));
    q.addBindValue(id);
    return q.exec();
}

// ---- received ----

bool SyncDatabase::isReceived(const QString &relPath) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM received WHERE rel_path = ?"));
    q.addBindValue(relPath);
    return q.exec() && q.next();
}

bool SyncDatabase::markReceived(const QString &relPath, const QString &hash)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO received (rel_path, hash, received_at) VALUES (?, ?, ?) "
        "ON CONFLICT(rel_path) DO UPDATE SET hash=excluded.hash, received_at=excluded.received_at"));
    q.addBindValue(relPath);
    q.addBindValue(hash);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!q.exec()) {
        Logger::get().warn("SyncDatabase: markReceived {} failed — {}",
                           relPath.toStdString(), q.lastError().text().toStdString());
        return false;
    }
    return true;
}

QSet<QString> SyncDatabase::receivedPaths() const
{
    QSet<QString> out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT rel_path FROM received")))
        return out;
    while (q.next())
        out.insert(q.value(0).toString());
    return out;
}

// ---- devices ----

bool SyncDatabase::upsertDevice(const DeviceRecord &rec)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO devices (device_id, fingerprint, secret, paired_at, last_seen_at) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(device_id) DO UPDATE SET fingerprint=excluded.fingerprint, "
        "secret=excluded.secret, paired_at=excluded.paired_at, last_seen_at=excluded.last_seen_at"));
    q.addBindValue(rec.deviceId);
    q.addBindValue(rec.fingerprint);
    q.addBindValue(rec.secret);
    q.addBindValue(rec.pairedAt);
    q.addBindValue(rec.lastSeenAt);
    if (!q.exec()) {
        Logger::get().warn("SyncDatabase: upsertDevice {} failed — {}",
                           rec.deviceId.toStdString(), q.lastError().text().toStdString());
        return false;
    }
    return true;
}

std::optional<DeviceRecord> SyncDatabase::device(const QString &deviceId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT device_id, fingerprint, secret, paired_at, last_seen_at "
        "FROM devices WHERE device_id = ?"));
    q.addBindValue(deviceId);
    if (!q.exec() || !q.next())
        return std::nullopt;

    DeviceRecord rec;
    rec.deviceId    = q.value(0).toString();
    rec.fingerprint = q.value(1).toString();
    rec.secret      = q.value(2).toByteArray();
    rec.pairedAt    = q.value(3).toLongLong();
    rec.lastSeenAt  = q.value(4).toLongLong();
    return rec;
}

QList<DeviceRecord> SyncDatabase::allDevices() const
{
    QList<DeviceRecord> out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT device_id, fingerprint, secret, paired_at, last_seen_at "
            "FROM devices ORDER BY paired_at DESC")))
        return out;

    while (q.next()) {
        DeviceRecord rec;
        rec.deviceId    = q.value(0).toString();
        rec.fingerprint = q.value(1).toString();
        rec.secret      = q.value(2).toByteArray();
        rec.pairedAt    = q.value(3).toLongLong();
        rec.lastSeenAt  = q.value(4).toLongLong();
        out.append(rec);
    }
    return out;
}

bool SyncDatabase::removeDevice(const QString &deviceId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM devices WHERE device_id = ?"));
    q.addBindValue(deviceId);
    return q.exec();
}

bool SyncDatabase::touchDevice(const QString &deviceId, qint64 seenAt)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE devices SET last_seen_at = ? WHERE device_id = ?"));
    q.addBindValue(seenAt);
    q.addBindValue(deviceId);
    return q.exec();
}
