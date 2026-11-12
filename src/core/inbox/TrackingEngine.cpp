#include "core/inbox/TrackingEngine.h"

#include "core/FileIdentity.h"
#include "core/FileIndex.h"
#include "core/FileEventDispatcher.h"
#include "core/Logger.h"
#include "database/DownloadsDatabase.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTimer>

namespace {
constexpr qint64 kRecentTtlMs = 15000;
}

TrackingEngine::TrackingEngine(DownloadsDatabase *db, FileIndex *index, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_index(index)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &TrackingEngine::tick);

    if (m_index) {
        // 等扫描完成再做离线反查（cacheLoaded 的快照可能还没包含移动后的新位置）
        connect(m_index, &FileIndex::indexReady, this, [this](int, qint64) {
            reconcileOffline();
        });
    }
}

TrackingEngine::~TrackingEngine()
{
    stop();
    if (m_dispatcher && m_subscription > 0)
        m_dispatcher->unsubscribe(m_subscription);
}

void TrackingEngine::setBoxPath(const QString &path)
{
    m_boxPath = QDir::fromNativeSeparators(path);
}

void TrackingEngine::setWindowMs(int ms)
{
    if (ms >= 200)
        m_windowMs = ms;
}

void TrackingEngine::setEventSource(FileEventDispatcher *dispatcher)
{
    if (m_dispatcher && m_subscription > 0)
        m_dispatcher->unsubscribe(m_subscription);

    m_dispatcher = dispatcher;
    m_subscription = 0;
    if (!m_dispatcher)
        return;

    m_subscription = m_dispatcher->subscribe([this](const FileSystemEvent &ev) {
        QVector<FileSystemEvent> one;
        one.append(ev);
        handleBatch(one);
    });
}

void TrackingEngine::start()
{
    if (m_running)
        return;
    m_running = true;
    m_timer->start();
    if (m_index && m_index->isReady())
        QTimer::singleShot(0, this, [this] { reconcileOffline(); });
}

void TrackingEngine::stop()
{
    if (m_timer)
        m_timer->stop();
    m_running = false;
}

QString TrackingEngine::normalized(const QString &path) const
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool TrackingEngine::isUnderBox(const QString &path) const
{
    if (m_boxPath.isEmpty())
        return false;
    const QString p = normalized(path).toLower();
    QString b = normalized(m_boxPath).toLower();
    while (b.size() > 3 && b.endsWith(QLatin1Char('/')))
        b.chop(1);
    return !b.isEmpty() && (p == b || p.startsWith(b + QLatin1Char('/')));
}

void TrackingEngine::handleBatch(const QVector<FileSystemEvent> &batch)
{
    if (!m_db || !m_db->isOpen())
        return;

    for (const FileSystemEvent &ev : batch) {
        switch (ev.type) {
        case FileSystemEvent::Removed:
            onRemoved(ev.path);
            break;
        case FileSystemEvent::Renamed:
            onRenamed(ev.oldPath, ev.path);
            break;
        case FileSystemEvent::Added:
        case FileSystemEvent::Modified:
            onAdded(ev.path);
            break;
        case FileSystemEvent::Overflow:
            // 事件不可信：窗口内待追踪记录直接落 Unknown，不猜
            for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
                markUnknown(it.value(), QStringLiteral("watcher-overflow"));
            m_pending.clear();
            m_recent.clear();
            break;
        }
    }
}

void TrackingEngine::onRemoved(const QString &path)
{
    if (isUnderBox(path)) {
        const auto row = m_db->byBoxPath(normalized(path));
        if (!row || !row->inBox)
            return;
        if (QFileInfo::exists(path))
            return;   // 事件抖动 / 已被重建：以物理存在为准
        startTracking(row->id, normalized(path));
        return;
    }

    // 多级移动：已归档的文件再次被移走
    const auto row = m_db->byCurPath(normalized(path));
    if (row && row->trackState == QLatin1String("FiledAway")) {
        if (!QFileInfo::exists(path))
            startTracking(row->id, normalized(path));
    }
}

void TrackingEngine::onRenamed(const QString &oldPath, const QString &newPath)
{
    const bool oldBox = isUnderBox(oldPath);
    const bool newBox = isUnderBox(newPath);

    if (oldBox && newBox) {
        // box 内改名：位置没变，不进入追踪
        const auto row = m_db->byBoxPath(normalized(oldPath));
        if (row && row->inBox) {
            m_db->updateBoxPath(row->id, normalized(newPath));
            emit changed();
        }
        return;
    }

    if (oldBox && !newBox) {
        const auto row = m_db->byBoxPath(normalized(oldPath));
        if (row && row->inBox) {
            startTracking(row->id, normalized(oldPath));
            tryCandidate(newPath);
        }
        return;
    }

    if (!oldBox && !newBox) {
        const auto row = m_db->byCurPath(normalized(oldPath));
        if (row && row->trackState == QLatin1String("FiledAway")) {
            startTracking(row->id, normalized(oldPath));
            tryCandidate(newPath);
        }
    }
}

void TrackingEngine::onAdded(const QString &path)
{
    if (isUnderBox(path))
        return;

    const QFileInfo info(path);
    if (!info.isFile())
        return;

    Candidate cand;
    cand.path = normalized(path);
    cand.size = info.size();
    cand.mtime = info.lastModified().toSecsSinceEpoch();
    cand.seenMs = QDateTime::currentMSecsSinceEpoch();

    FileIdentity::Identity identity;
    if (FileIdentity::read(path, &identity)) {
        cand.volumeSerial = identity.volumeSerial;
        cand.fileId = identity.fileId;
    }

    m_recent.insert(cand.path.toLower(), cand);
    tryCandidate(cand.path);
}

void TrackingEngine::startTracking(int downloadId, const QString &fromPath)
{
    if (m_pending.contains(downloadId))
        return;

    const auto row = m_db->byId(downloadId);
    if (!row)
        return;

    Pending p;
    p.downloadId = downloadId;
    p.fromPath = fromPath;
    p.deadlineMs = QDateTime::currentMSecsSinceEpoch() + m_windowMs;
    p.target.volumeSerial = row->volumeSerial;
    p.target.fileId = row->fileId;
    p.target.size = row->size;
    p.target.mtime = row->mtime;
    p.target.sigQuick = row->sigQuick;

    m_pending.insert(downloadId, p);
    m_db->updateTrackState(downloadId, QStringLiteral("Tracking"), fromPath, false);
    Logger::get().info("TrackingEngine: 开始追踪 {}（{} 秒窗口）",
                       row->name.toStdString(), m_windowMs / 1000);
    emit changed();

    // 跨卷复制时，目标盘的 Added 可能先于 box 的 Removed 到达 —— 先扫最近候选
    const QStringList keys = m_recent.keys();
    for (const QString &key : keys)
        tryCandidate(m_recent.value(key).path);
}

TrackingEngine::MatchKind TrackingEngine::matchKind(const Target &target,
                                                    const Candidate &cand,
                                                    const QString &sig) const
{
    if (target.size != cand.size || target.mtime != cand.mtime)
        return MatchKind::None;

    if (!target.fileId.isEmpty() && !cand.fileId.isEmpty()
        && target.volumeSerial == cand.volumeSerial
        && target.fileId == cand.fileId) {
        return MatchKind::Exact;
    }

    if (!target.sigQuick.isEmpty() && !sig.isEmpty() && target.sigQuick == sig)
        return MatchKind::Strong;

    return MatchKind::None;
}

void TrackingEngine::tryCandidate(const QString &path)
{
    if (m_pending.isEmpty() || isUnderBox(path))
        return;

    const QFileInfo info(path);
    if (!info.isFile())
        return;

    Candidate cand;
    cand.path = normalized(path);
    cand.size = info.size();
    cand.mtime = info.lastModified().toSecsSinceEpoch();

    FileIdentity::Identity identity;
    if (FileIdentity::read(path, &identity)) {
        cand.volumeSerial = identity.volumeSerial;
        cand.fileId = identity.fileId;
    }

    QString sig;   // 懒计算：只有 size/mtime 命中且 FileId 不同才需要
    QList<int> matches;
    QHash<int, MatchKind> kinds;

    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        const Target &target = it.value().target;
        if (target.size != cand.size || target.mtime != cand.mtime)
            continue;
        if (sig.isEmpty() && !target.sigQuick.isEmpty())
            sig = FileIdentity::quickSignature(path);

        const MatchKind kind = matchKind(target, cand, sig);
        if (kind != MatchKind::None) {
            matches.append(it.key());
            kinds.insert(it.key(), kind);
        }
    }

    if (matches.size() != 1)
        return;   // 0 = 不是；>1 = 多候选，绝不猜

    const int id = matches.first();
    Pending p = m_pending.value(id);
    const MatchKind kind = kinds.value(id);
    confirm(p,
            cand.path,
            kind == MatchKind::Exact ? QStringLiteral("watcher-frn")
                                     : QStringLiteral("watcher-sig"),
            kind == MatchKind::Exact ? QStringLiteral("exact")
                                     : QStringLiteral("strong"));
    m_pending.remove(id);
    m_recent.remove(cand.path.toLower());
}

void TrackingEngine::confirm(Pending &pending, const QString &toPath,
                             const QString &method, const QString &confidence)
{
    const auto row = m_db->byId(pending.downloadId);
    const QString name = row ? row->name : QString();

    m_db->updateTrackState(pending.downloadId, QStringLiteral("FiledAway"),
                           toPath, false,
                           QDateTime::currentDateTime().toString(Qt::ISODate));
    m_db->addTrackingEvent(pending.downloadId, pending.fromPath, toPath, method, confidence);

    Logger::get().info("TrackingEngine: {} → {}（{} / {}）",
                       name.toStdString(), toPath.toStdString(),
                       method.toStdString(), confidence.toStdString());
    emit notice(QStringLiteral("%1 → %2").arg(name, QDir::toNativeSeparators(toPath)));
    emit changed();
}

void TrackingEngine::markUnknown(Pending &pending, const QString &method)
{
    const auto row = m_db->byId(pending.downloadId);
    const QString name = row ? row->name : QString();

    m_db->updateTrackState(pending.downloadId, QStringLiteral("Unknown"),
                           pending.fromPath, false,
                           QDateTime::currentDateTime().toString(Qt::ISODate));
    m_db->addTrackingEvent(pending.downloadId, pending.fromPath, QString(), method,
                           QStringLiteral("unknown"));
    Logger::get().warn("TrackingEngine: 未能定位 {}（{}）", name.toStdString(), method.toStdString());
    emit changed();
}

void TrackingEngine::tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (auto it = m_recent.begin(); it != m_recent.end();) {
        if (now - it.value().seenMs > kRecentTtlMs)
            it = m_recent.erase(it);
        else
            ++it;
    }

    QList<int> expired;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        if (now >= it.value().deadlineMs)
            expired.append(it.key());
    }
    for (int id : expired) {
        Pending p = m_pending.take(id);
        markUnknown(p, QStringLiteral("watcher-timeout"));
    }
}

void TrackingEngine::reconcileOffline()
{
    if (m_offlineDone || !m_db || !m_db->isOpen())
        return;
    if (!m_index || !m_index->isReady())
        return;
    m_offlineDone = true;

    const QVector<DownloadRow> rows = m_db->listInBox();
    if (rows.isEmpty())
        return;

    // 级别 0：box 内被改名（FileId 不变）
    QHash<QString, QString> byFileId;
    for (auto it = QDirIterator(m_boxPath, QDir::Files, QDirIterator::Subdirectories);
         it.hasNext();) {
        const QString path = it.next();
        FileIdentity::Identity identity;
        if (FileIdentity::read(path, &identity) && !identity.fileId.isEmpty())
            byFileId.insert(identity.fileId, path);
    }

    int moved = 0;
    int unknown = 0;
    for (const DownloadRow &row : rows) {
        if (!row.boxPath.isEmpty() && QFileInfo::exists(row.boxPath))
            continue;

        if (!row.fileId.isEmpty() && byFileId.contains(row.fileId)) {
            m_db->updateBoxPath(row.id, byFileId.value(row.fileId));
            ++moved;
            continue;
        }

        // 级别 1：(size, mtime) 反查 + quickSig 唯一确认
        const QStringList candidates = m_index->pathsBySizeMtime(row.size, row.mtime);
        QStringList hits;
        for (const QString &candidate : candidates) {
            if (row.sigQuick.isEmpty())
                break;
            if (FileIdentity::quickSignature(candidate) == row.sigQuick)
                hits.append(candidate);
        }

        if (hits.size() == 1) {
            m_db->updateTrackState(row.id, QStringLiteral("FiledAway"), hits.first(), false,
                                   QDateTime::currentDateTime().toString(Qt::ISODate));
            m_db->addTrackingEvent(row.id, row.boxPath, hits.first(),
                                   QStringLiteral("startup-rescan"), QStringLiteral("strong"));
            ++moved;
        } else {
            m_db->updateTrackState(row.id, QStringLiteral("Unknown"), row.boxPath, false,
                                   QDateTime::currentDateTime().toString(Qt::ISODate));
            m_db->addTrackingEvent(row.id, row.boxPath, QString(),
                                   QStringLiteral("startup-rescan"), QStringLiteral("unknown"));
            ++unknown;
        }
    }

    if (moved > 0 || unknown > 0) {
        emit notice(QStringLiteral("启动对账：%1 个已归档，%2 个位置未知")
                        .arg(moved).arg(unknown));
        emit changed();
    }
}
