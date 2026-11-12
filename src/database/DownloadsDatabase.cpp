#include "database/DownloadsDatabase.h"
#include "database/DatabaseMigrator.h"
#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

const char *kDownloadColumns =
    "id, source_id, src_path, box_path, cur_path, in_box, name, ext, cat_mask, size, mtime,"
    " vol_serial, file_id, sig_quick, sig_full, url, referrer, pipe_state, track_state,"
    " detected_by, dup_of, error_code, tags, suggest_dest, rule_name, created_at,"
    " completed_at, fetched_at, filed_at, last_verified";

QString nowIso()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

DownloadRow rowFromQuery(const QSqlQuery &q)
{
    DownloadRow r;
    int i = 0;
    r.id            = q.value(i++).toInt();
    r.sourceId      = q.value(i++).toString();
    r.srcPath       = q.value(i++).toString();
    r.boxPath       = q.value(i++).toString();
    r.curPath       = q.value(i++).toString();
    r.inBox         = q.value(i++).toInt() != 0;
    r.name          = q.value(i++).toString();
    r.ext           = q.value(i++).toString();
    r.catMask       = static_cast<uint16_t>(q.value(i++).toInt());
    r.size          = q.value(i++).toLongLong();
    r.mtime         = q.value(i++).toLongLong();
    r.volumeSerial  = static_cast<quint32>(q.value(i++).toUInt());
    r.fileId        = q.value(i++).toString();
    r.sigQuick      = q.value(i++).toString();
    r.sigFull       = q.value(i++).toString();
    r.url           = q.value(i++).toString();
    r.referrer      = q.value(i++).toString();
    r.pipeState     = q.value(i++).toString();
    r.trackState    = q.value(i++).toString();
    r.detectedBy    = q.value(i++).toString();
    r.dupOf         = q.value(i++).toInt();
    r.errorCode     = q.value(i++).toInt();
    r.tags          = q.value(i++).toString();
    r.suggestDest   = q.value(i++).toString();
    r.ruleName      = q.value(i++).toString();
    r.createdAt     = q.value(i++).toString();
    r.completedAt   = q.value(i++).toString();
    r.fetchedAt     = q.value(i++).toString();
    r.filedAt       = q.value(i++).toString();
    r.lastVerified  = q.value(i++).toString();
    return r;
}

bool createSchemaV1(QSqlDatabase &db)
{
    auto exec = [&db](const char *sql, const char *what) {
        QSqlQuery q(db);
        if (q.exec(QString::fromLatin1(sql)))
            return true;
        Logger::get().error("DownloadsDatabase: 建表失败（{}）— {}",
                            what, q.lastError().text().toStdString());
        return false;
    };

    if (!exec(
            "CREATE TABLE IF NOT EXISTS downloads ("
            "  id           INTEGER PRIMARY KEY,"
            "  source_id    TEXT NOT NULL,"
            "  src_path     TEXT NOT NULL,"
            "  box_path     TEXT,"
            "  cur_path     TEXT,"
            "  in_box       INTEGER NOT NULL DEFAULT 0,"
            "  name         TEXT NOT NULL,"
            "  ext          TEXT,"
            "  cat_mask     INTEGER NOT NULL DEFAULT 0,"
            "  size         INTEGER NOT NULL DEFAULT 0,"
            "  mtime        INTEGER NOT NULL DEFAULT 0,"
            "  vol_serial   INTEGER,"
            "  file_id      TEXT,"
            "  sig_quick    TEXT,"
            "  sig_full     TEXT,"
            "  url          TEXT,"
            "  referrer     TEXT,"
            "  pipe_state   TEXT NOT NULL,"
            "  track_state  TEXT NOT NULL,"
            "  detected_by  TEXT,"
            "  dup_of       INTEGER,"
            "  error_code   INTEGER,"
            "  tags         TEXT,"
            "  created_at   TEXT NOT NULL,"
            "  completed_at TEXT,"
            "  fetched_at   TEXT,"
            "  filed_at     TEXT,"
            "  last_verified TEXT"
            ")", "downloads"))
        return false;

    if (!exec("CREATE INDEX IF NOT EXISTS idx_dl_pipe ON downloads(pipe_state)", "idx_dl_pipe")
        || !exec("CREATE INDEX IF NOT EXISTS idx_dl_track ON downloads(track_state, in_box)", "idx_dl_track")
        || !exec("CREATE INDEX IF NOT EXISTS idx_dl_created ON downloads(created_at DESC)", "idx_dl_created")
        || !exec("CREATE INDEX IF NOT EXISTS idx_dl_sig ON downloads(sig_quick)", "idx_dl_sig")
        || !exec("CREATE INDEX IF NOT EXISTS idx_dl_cur ON downloads(cur_path)", "idx_dl_cur")
        || !exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_dl_src ON downloads(src_path)", "idx_dl_src"))
        return false;

    if (!exec(
            "CREATE TABLE IF NOT EXISTS move_journal ("
            "  id          INTEGER PRIMARY KEY,"
            "  download_id INTEGER REFERENCES downloads(id),"
            "  batch_id    TEXT,"
            "  op          TEXT NOT NULL,"
            "  from_path   TEXT NOT NULL,"
            "  to_path     TEXT,"
            "  op_state    TEXT NOT NULL,"
            "  error_code  INTEGER,"
            "  at          TEXT NOT NULL"
            ")", "move_journal"))
        return false;
    if (!exec("CREATE INDEX IF NOT EXISTS idx_journal_batch ON move_journal(batch_id)", "idx_journal_batch")
        || !exec("CREATE INDEX IF NOT EXISTS idx_journal_state ON move_journal(op_state)", "idx_journal_state"))
        return false;

    if (!exec(
            "CREATE TABLE IF NOT EXISTS tracking_events ("
            "  id          INTEGER PRIMARY KEY,"
            "  download_id INTEGER NOT NULL REFERENCES downloads(id),"
            "  from_path   TEXT NOT NULL,"
            "  to_path     TEXT,"
            "  method      TEXT NOT NULL,"
            "  confidence  TEXT NOT NULL,"
            "  at          TEXT NOT NULL"
            ")", "tracking_events"))
        return false;
    if (!exec("CREATE INDEX IF NOT EXISTS idx_track_dl ON tracking_events(download_id, at)", "idx_track_dl"))
        return false;

    if (!exec(
            "CREATE TABLE IF NOT EXISTS download_rules ("
            "  id         INTEGER PRIMARY KEY,"
            "  name       TEXT NOT NULL,"
            "  enabled    INTEGER NOT NULL DEFAULT 1,"
            "  priority   INTEGER NOT NULL DEFAULT 100,"
            "  match_json TEXT NOT NULL,"
            "  action     TEXT NOT NULL,"
            "  dest       TEXT,"
            "  hit_count  INTEGER NOT NULL DEFAULT 0"
            ")", "download_rules"))
        return false;

    if (!exec(
            "CREATE TABLE IF NOT EXISTS sources ("
            "  source_id  TEXT PRIMARY KEY,"
            "  enabled    INTEGER NOT NULL DEFAULT 0,"
            "  paths_json TEXT,"
            "  last_seen  TEXT"
            ")", "sources"))
        return false;

    if (!exec(
            "CREATE TABLE IF NOT EXISTS app_meta ("
            "  key   TEXT PRIMARY KEY,"
            "  value TEXT"
            ")", "app_meta"))
        return false;

    return true;
}

/// v1 → v2：归档建议（S11）。只加列，不重建。
bool migrateToV2(QSqlDatabase &db)
{
    auto exec = [&db](const char *sql) {
        QSqlQuery q(db);
        if (q.exec(QString::fromLatin1(sql)))
            return true;
        Logger::get().error("DownloadsDatabase: 迁移 v2 失败 — {}",
                            q.lastError().text().toStdString());
        return false;
    };
    return exec("ALTER TABLE downloads ADD COLUMN suggest_dest TEXT")
        && exec("ALTER TABLE downloads ADD COLUMN rule_name TEXT");
}

} // namespace

DownloadsDatabase::DownloadsDatabase(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

DownloadsDatabase::~DownloadsDatabase()
{
    close();
}

bool DownloadsDatabase::open(const QString &filePath)
{
    close();
    m_path = filePath;
    QDir().mkpath(QFileInfo(filePath).absolutePath());

    m_db = new QSqlDatabase(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName));
    m_db->setDatabaseName(filePath);

    if (!m_db->open()) {
        m_lastError = m_db->lastError().text();
        Logger::get().error("DownloadsDatabase: 打不开 {} — {}",
                            filePath.toStdString(), m_lastError.toStdString());
        close();
        return false;
    }

    QSqlQuery pragma(*m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    if (!ensureSchema()) {
        close();
        return false;
    }

    Logger::get().info("DownloadsDatabase: opened {} ({} rows)",
                       filePath.toStdString(), countAll());
    return true;
}

void DownloadsDatabase::close()
{
    if (m_db) {
        if (m_db->isOpen())
            m_db->close();
        delete m_db;
        m_db = nullptr;
    }
    if (!m_connectionName.isEmpty() && QSqlDatabase::contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool DownloadsDatabase::isOpen() const
{
    return m_db && m_db->isOpen();
}

bool DownloadsDatabase::exec(QSqlDatabase &db, const QString &sql, const char *what)
{
    QSqlQuery q(db);
    if (q.exec(sql))
        return true;
    m_lastError = q.lastError().text();
    Logger::get().error("DownloadsDatabase: {} 失败 — {}", what, m_lastError.toStdString());
    return false;
}

bool DownloadsDatabase::ensureSchema()
{
    DatabaseMigrator migrator(QStringLiteral("downloads"), {
        { 1, QStringLiteral("建 downloads / move_journal / tracking_events / rules / sources / meta"),
          &createSchemaV1 },
        { 2, QStringLiteral("加 suggest_dest / rule_name（S11 归档建议）"), &migrateToV2 },
    });

    if (migrator.run(*m_db))
        return true;

    m_lastError = QStringLiteral("schema 迁移失败（downloads.db 是权威数据，拒绝重建）");
    return false;
}

// ---------------------------------------------------------------------------
// downloads
// ---------------------------------------------------------------------------

int DownloadsDatabase::insert(const DownloadRow &row)
{
    if (!isOpen())
        return -1;

    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        " INSERT INTO downloads (source_id, src_path, box_path, cur_path, in_box, name, ext,"
        " cat_mask, size, mtime, vol_serial, file_id, sig_quick, sig_full, url, referrer,"
        " pipe_state, track_state, detected_by, dup_of, error_code, tags, suggest_dest,"
        " rule_name, created_at, completed_at, fetched_at, filed_at, last_verified)"
        " VALUES (:source_id, :src_path, :box_path, :cur_path, :in_box, :name, :ext,"
        " :cat_mask, :size, :mtime, :vol_serial, :file_id, :sig_quick, :sig_full, :url, :referrer,"
        " :pipe_state, :track_state, :detected_by, :dup_of, :error_code, :tags, :suggest_dest,"
        " :rule_name, :created_at, :completed_at, :fetched_at, :filed_at, :last_verified)"));

    q.bindValue(QStringLiteral(":source_id"), row.sourceId);
    q.bindValue(QStringLiteral(":src_path"), row.srcPath);
    q.bindValue(QStringLiteral(":box_path"), row.boxPath);
    q.bindValue(QStringLiteral(":cur_path"), row.curPath);
    q.bindValue(QStringLiteral(":in_box"), row.inBox ? 1 : 0);
    q.bindValue(QStringLiteral(":name"), row.name);
    q.bindValue(QStringLiteral(":ext"), row.ext);
    q.bindValue(QStringLiteral(":cat_mask"), static_cast<int>(row.catMask));
    q.bindValue(QStringLiteral(":size"), row.size);
    q.bindValue(QStringLiteral(":mtime"), row.mtime);
    q.bindValue(QStringLiteral(":vol_serial"), static_cast<uint>(row.volumeSerial));
    q.bindValue(QStringLiteral(":file_id"), row.fileId);
    q.bindValue(QStringLiteral(":sig_quick"), row.sigQuick);
    q.bindValue(QStringLiteral(":sig_full"), row.sigFull);
    q.bindValue(QStringLiteral(":url"), row.url);
    q.bindValue(QStringLiteral(":referrer"), row.referrer);
    q.bindValue(QStringLiteral(":pipe_state"), row.pipeState);
    q.bindValue(QStringLiteral(":track_state"), row.trackState);
    q.bindValue(QStringLiteral(":detected_by"), row.detectedBy);
    q.bindValue(QStringLiteral(":dup_of"), row.dupOf);
    q.bindValue(QStringLiteral(":error_code"), row.errorCode);
    q.bindValue(QStringLiteral(":tags"), row.tags);
    q.bindValue(QStringLiteral(":suggest_dest"), row.suggestDest);
    q.bindValue(QStringLiteral(":rule_name"), row.ruleName);
    q.bindValue(QStringLiteral(":created_at"),
                row.createdAt.isEmpty() ? nowIso() : row.createdAt);
    q.bindValue(QStringLiteral(":completed_at"), row.completedAt);
    q.bindValue(QStringLiteral(":fetched_at"), row.fetchedAt);
    q.bindValue(QStringLiteral(":filed_at"), row.filedAt);
    q.bindValue(QStringLiteral(":last_verified"), row.lastVerified);

    if (!q.exec()) {
        m_lastError = q.lastError().text();
        Logger::get().warn("DownloadsDatabase: insert 失败 — {}", m_lastError.toStdString());
        return -1;
    }
    return q.lastInsertId().toInt();
}

bool DownloadsDatabase::updateAfterFetch(int id, const QString &boxPath, qint64 size,
                                         qint64 mtime, quint32 volumeSerial,
                                         const QString &fileId, const QString &quickSig)
{
    if (!isOpen())
        return false;

    const QString now = nowIso();
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET box_path=:box, cur_path=:box, in_box=1, size=:size,"
        " mtime=:mtime, vol_serial=:vol, file_id=:fid, sig_quick=:sig,"
        " pipe_state='InBox', track_state='InBox', fetched_at=:now, completed_at=:now,"
        " last_verified=:now WHERE id=:id"));
    q.bindValue(QStringLiteral(":box"), boxPath);
    q.bindValue(QStringLiteral(":size"), size);
    q.bindValue(QStringLiteral(":mtime"), mtime);
    q.bindValue(QStringLiteral(":vol"), static_cast<uint>(volumeSerial));
    q.bindValue(QStringLiteral(":fid"), fileId);
    q.bindValue(QStringLiteral(":sig"), quickSig);
    q.bindValue(QStringLiteral(":now"), now);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool DownloadsDatabase::updatePipeState(int id, const QString &state, int errorCode)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET pipe_state=:s, error_code=:e WHERE id=:id"));
    q.bindValue(QStringLiteral(":s"), state);
    q.bindValue(QStringLiteral(":e"), errorCode);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool DownloadsDatabase::updateTrackState(int id, const QString &state, const QString &curPath,
                                         bool inBox, const QString &filedAt)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET track_state=:s, cur_path=:p, in_box=:b,"
        " filed_at=COALESCE(NULLIF(:f,''), filed_at), last_verified=:now WHERE id=:id"));
    q.bindValue(QStringLiteral(":s"), state);
    q.bindValue(QStringLiteral(":p"), curPath);
    q.bindValue(QStringLiteral(":b"), inBox ? 1 : 0);
    q.bindValue(QStringLiteral(":f"), filedAt);
    q.bindValue(QStringLiteral(":now"), nowIso());
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool DownloadsDatabase::updatePaths(int id, const QString &srcPath, const QString &curPath)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("UPDATE downloads SET src_path=:s, cur_path=:c WHERE id=:id"));
    q.bindValue(QStringLiteral(":s"), srcPath);
    q.bindValue(QStringLiteral(":c"), curPath);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool DownloadsDatabase::updateBoxPath(int id, const QString &boxPath)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET box_path=:b, cur_path=CASE WHEN in_box=1 THEN :b ELSE cur_path END"
        " WHERE id=:id"));
    q.bindValue(QStringLiteral(":b"), boxPath);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

std::optional<DownloadRow> DownloadsDatabase::byId(int id) const
{
    if (!isOpen())
        return std::nullopt;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM downloads WHERE id=:id")
                  .arg(QString::fromLatin1(kDownloadColumns)));
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return rowFromQuery(q);
}

std::optional<DownloadRow> DownloadsDatabase::bySrcPath(const QString &srcPath) const
{
    if (!isOpen())
        return std::nullopt;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM downloads WHERE src_path=:p")
                  .arg(QString::fromLatin1(kDownloadColumns)));
    q.bindValue(QStringLiteral(":p"), srcPath);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return rowFromQuery(q);
}

std::optional<DownloadRow> DownloadsDatabase::byBoxPath(const QString &boxPath) const
{
    if (!isOpen())
        return std::nullopt;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM downloads WHERE box_path=:p")
                  .arg(QString::fromLatin1(kDownloadColumns)));
    q.bindValue(QStringLiteral(":p"), boxPath);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return rowFromQuery(q);
}

bool DownloadsDatabase::updateAfterArchive(int id, const QString &curPath)
{
    if (!isOpen())
        return false;
    const QString now = nowIso();
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET cur_path=:p, in_box=0, track_state='FiledAway',"
        " filed_at=:now, last_verified=:now, suggest_dest='' WHERE id=:id"));
    q.bindValue(QStringLiteral(":p"), curPath);
    q.bindValue(QStringLiteral(":now"), now);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool DownloadsDatabase::updateAfterUnarchive(int id, const QString &boxPath)
{
    if (!isOpen())
        return false;
    const QString now = nowIso();
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET box_path=:p, cur_path=:p, in_box=1,"
        " pipe_state='InBox', track_state='InBox', filed_at=NULL,"
        " last_verified=:now, suggest_dest='' WHERE id=:id"));
    q.bindValue(QStringLiteral(":p"), boxPath);
    q.bindValue(QStringLiteral(":now"), now);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool DownloadsDatabase::setSuggestion(int id, const QString &dest, const QString &ruleName)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET suggest_dest=:d, rule_name=:r WHERE id=:id"));
    q.bindValue(QStringLiteral(":d"), dest);
    q.bindValue(QStringLiteral(":r"), ruleName);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool DownloadsDatabase::setDupOf(int id, int dupOf)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("UPDATE downloads SET dup_of=:d WHERE id=:id"));
    q.bindValue(QStringLiteral(":d"), dupOf);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool DownloadsDatabase::updateAfterDelete(int id)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET in_box=0, pipe_state='Deleted', track_state='Deleted',"
        " last_verified=:now, suggest_dest='' WHERE id=:id"));
    q.bindValue(QStringLiteral(":now"), nowIso());
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

std::optional<DownloadRow> DownloadsDatabase::bySigQuick(const QString &sigQuick,
                                                         int excludeId) const
{
    if (!isOpen() || sigQuick.isEmpty())
        return std::nullopt;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "SELECT %1 FROM downloads WHERE sig_quick=:s AND id!=:id ORDER BY id ASC LIMIT 1")
                  .arg(QString::fromLatin1(kDownloadColumns)));
    q.bindValue(QStringLiteral(":s"), sigQuick);
    q.bindValue(QStringLiteral(":id"), excludeId);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return rowFromQuery(q);
}

std::optional<DownloadRow> DownloadsDatabase::byCurPath(const QString &curPath) const
{
    if (!isOpen())
        return std::nullopt;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM downloads WHERE cur_path=:p ORDER BY id DESC LIMIT 1")
                  .arg(QString::fromLatin1(kDownloadColumns)));
    q.bindValue(QStringLiteral(":p"), curPath);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return rowFromQuery(q);
}

QVector<DownloadRow> DownloadsDatabase::list(int limit) const
{
    QVector<DownloadRow> rows;
    if (!isOpen() || limit <= 0)
        return rows;

    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM downloads ORDER BY id DESC LIMIT :n")
                  .arg(QString::fromLatin1(kDownloadColumns)));
    q.bindValue(QStringLiteral(":n"), limit);
    if (!q.exec())
        return rows;
    while (q.next())
        rows.append(rowFromQuery(q));
    return rows;
}

QVector<DownloadRow> DownloadsDatabase::listInBox() const
{
    QVector<DownloadRow> rows;
    if (!isOpen())
        return rows;

    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM downloads WHERE in_box=1 ORDER BY id DESC")
                  .arg(QString::fromLatin1(kDownloadColumns)));
    if (!q.exec())
        return rows;
    while (q.next())
        rows.append(rowFromQuery(q));
    return rows;
}

int DownloadsDatabase::countInBox() const
{
    if (!isOpen())
        return 0;
    QSqlQuery q(*m_db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM downloads WHERE in_box=1")) || !q.next())
        return 0;
    return q.value(0).toInt();
}

int DownloadsDatabase::countAll() const
{
    if (!isOpen())
        return 0;
    QSqlQuery q(*m_db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM downloads")) || !q.next())
        return 0;
    return q.value(0).toInt();
}

// ---------------------------------------------------------------------------
// move_journal
// ---------------------------------------------------------------------------

qint64 DownloadsDatabase::beginJournal(int downloadId, const QString &op,
                                       const QString &fromPath, const QString &toPath)
{
    if (!isOpen())
        return -1;

    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO move_journal (download_id, op, from_path, to_path, op_state, at)"
        " VALUES (:dl, :op, :from, :to, 'intent', :at)"));
    q.bindValue(QStringLiteral(":dl"), downloadId);
    q.bindValue(QStringLiteral(":op"), op);
    q.bindValue(QStringLiteral(":from"), fromPath);
    q.bindValue(QStringLiteral(":to"), toPath);
    q.bindValue(QStringLiteral(":at"), nowIso());
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool DownloadsDatabase::finishJournal(qint64 journalId, bool ok, int errorCode)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "UPDATE move_journal SET op_state=:s, error_code=:e WHERE id=:id"));
    q.bindValue(QStringLiteral(":s"), ok ? QStringLiteral("done") : QStringLiteral("failed"));
    q.bindValue(QStringLiteral(":e"), errorCode);
    q.bindValue(QStringLiteral(":id"), journalId);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

namespace {

JournalRow journalFromQuery(const QSqlQuery &q)
{
    JournalRow r;
    r.id = q.value(0).toLongLong();
    r.downloadId = q.value(1).toInt();
    r.batchId = q.value(2).toString();
    r.op = q.value(3).toString();
    r.fromPath = q.value(4).toString();
    r.toPath = q.value(5).toString();
    r.opState = q.value(6).toString();
    r.errorCode = q.value(7).toInt();
    r.at = q.value(8).toString();
    return r;
}

} // namespace

QVector<JournalRow> DownloadsDatabase::pendingIntents() const
{
    QVector<JournalRow> rows;
    if (!isOpen())
        return rows;
    QSqlQuery q(*m_db);
    if (!q.exec(QStringLiteral(
            "SELECT id, download_id, batch_id, op, from_path, to_path, op_state, error_code, at"
            " FROM move_journal WHERE op_state='intent' ORDER BY id")))
        return rows;
    while (q.next())
        rows.append(journalFromQuery(q));
    return rows;
}

QVector<JournalRow> DownloadsDatabase::journalFor(int downloadId) const
{
    QVector<JournalRow> rows;
    if (!isOpen())
        return rows;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "SELECT id, download_id, batch_id, op, from_path, to_path, op_state, error_code, at"
        " FROM move_journal WHERE download_id=:id ORDER BY id"));
    q.bindValue(QStringLiteral(":id"), downloadId);
    if (!q.exec())
        return rows;
    while (q.next())
        rows.append(journalFromQuery(q));
    return rows;
}

QVector<QStringList> DownloadsDatabase::trackingFor(int downloadId) const
{
    QVector<QStringList> rows;
    if (!isOpen())
        return rows;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "SELECT from_path, COALESCE(to_path,''), method, confidence, at"
        " FROM tracking_events WHERE download_id=:id ORDER BY id"));
    q.bindValue(QStringLiteral(":id"), downloadId);
    if (!q.exec())
        return rows;
    while (q.next()) {
        rows.append({ q.value(0).toString(), q.value(1).toString(),
                      q.value(2).toString(), q.value(3).toString(), q.value(4).toString() });
    }
    return rows;
}

qint64 DownloadsDatabase::addTrackingEvent(int downloadId, const QString &fromPath,
                                           const QString &toPath,
                                           const QString &method, const QString &confidence)
{
    if (!isOpen())
        return -1;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO tracking_events(download_id, from_path, to_path, method, confidence, at)"
        " VALUES(:id, :from, :to, :method, :confidence, :at)"));
    q.bindValue(QStringLiteral(":id"), downloadId);
    q.bindValue(QStringLiteral(":from"), fromPath);
    q.bindValue(QStringLiteral(":to"), toPath.isEmpty() ? QVariant() : toPath);
    q.bindValue(QStringLiteral(":method"), method);
    q.bindValue(QStringLiteral(":confidence"), confidence);
    q.bindValue(QStringLiteral(":at"), nowIso());
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

// ---------------------------------------------------------------------------
// app_meta / sources
// ---------------------------------------------------------------------------

QString DownloadsDatabase::meta(const QString &key) const
{
    if (!isOpen())
        return QString();
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT value FROM app_meta WHERE key=:k"));
    q.bindValue(QStringLiteral(":k"), key);
    if (!q.exec() || !q.next())
        return QString();
    return q.value(0).toString();
}

bool DownloadsDatabase::setMeta(const QString &key, const QString &value)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO app_meta(key, value) VALUES(:k, :v)"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    q.bindValue(QStringLiteral(":k"), key);
    q.bindValue(QStringLiteral(":v"), value);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

std::optional<DownloadsDatabase::SourceRow> DownloadsDatabase::source(const QString &sourceId) const
{
    if (!isOpen())
        return std::nullopt;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT source_id, enabled, paths_json, last_seen FROM sources WHERE source_id=:id"));
    q.bindValue(QStringLiteral(":id"), sourceId);
    if (!q.exec() || !q.next())
        return std::nullopt;
    SourceRow row;
    row.sourceId = q.value(0).toString();
    row.enabled = q.value(1).toInt() != 0;
    row.pathsJson = q.value(2).toString();
    row.lastSeen = q.value(3).toString();
    return row;
}

bool DownloadsDatabase::upsertSource(const SourceRow &row)
{
    if (!isOpen())
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO sources(source_id, enabled, paths_json, last_seen)"
        " VALUES(:id, :enabled, :paths, :seen)"
        " ON CONFLICT(source_id) DO UPDATE SET enabled=excluded.enabled,"
        " paths_json=excluded.paths_json, last_seen=excluded.last_seen"));
    q.bindValue(QStringLiteral(":id"), row.sourceId);
    q.bindValue(QStringLiteral(":enabled"), row.enabled ? 1 : 0);
    q.bindValue(QStringLiteral(":paths"), row.pathsJson);
    q.bindValue(QStringLiteral(":seen"), row.lastSeen);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}
