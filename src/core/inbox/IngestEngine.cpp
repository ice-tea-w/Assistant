#include "core/inbox/IngestEngine.h"

#include "core/FileClassifier.h"
#include "core/FileEventDispatcher.h"
#include "core/FileIdentity.h"
#include "core/Logger.h"
#include "core/inbox/IDownloadSource.h"
#include "database/DownloadsDatabase.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr int kTickMs = 300;
constexpr int kMaxExclusiveAttempts = 5;
constexpr qint64 kBackoffMs[kMaxExclusiveAttempts] = { 1000, 2000, 5000, 10000, 30000 };

QString toLongPath(const QString &path)
{
    QString win = path;
    win.replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (win.startsWith(QStringLiteral("\\\\?\\")))
        return win;
    if (win.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + win.mid(2);
    return QStringLiteral("\\\\?\\") + win;
}

bool moveFile(const QString &from, const QString &to, DWORD *error)
{
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(toLongPath(from).utf16()),
                    reinterpret_cast<LPCWSTR>(toLongPath(to).utf16()),
                    MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    if (error)
        *error = GetLastError();
    return false;
}

bool hasEnoughSpace(const QString &from, const QString &toDir, qint64 bytes)
{
    wchar_t fromVolume[MAX_PATH] = {};
    wchar_t toVolume[MAX_PATH] = {};
    if (!GetVolumePathNameW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(from).utf16()),
                            fromVolume, MAX_PATH)
        || !GetVolumePathNameW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(toDir).utf16()),
                               toVolume, MAX_PATH)) {
        return true;   // 判不了就不阻止（宁可让 MoveFileEx 自己失败）
    }
    if (QString::fromWCharArray(fromVolume).compare(QString::fromWCharArray(toVolume),
                                                    Qt::CaseInsensitive) == 0) {
        return true;   // 同卷只是 rename，不占新空间
    }

    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(reinterpret_cast<LPCWSTR>(toVolume), &freeBytes, nullptr, nullptr))
        return true;
    return static_cast<qint64>(freeBytes.QuadPart) > bytes;
}

QString nowIso()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

} // namespace

IngestEngine::IngestEngine(DownloadsDatabase *db, QObject *parent)
    : QObject(parent)
    , m_db(db)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &IngestEngine::tick);
}

IngestEngine::~IngestEngine()
{
    stop();
    if (m_dispatcher && m_subscription > 0)
        m_dispatcher->unsubscribe(m_subscription);
}

void IngestEngine::setSource(const IDownloadSource *source)
{
    m_source = source;
    m_candidates.clear();
}

void IngestEngine::setBoxPath(const QString &path)
{
    m_boxPath = QDir::fromNativeSeparators(path);
}

void IngestEngine::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!m_enabled)
        m_candidates.clear();
    Logger::get().info("IngestEngine: {}", enabled ? "已启用" : "已停用");
}

void IngestEngine::setEventSource(FileEventDispatcher *dispatcher)
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

bool IngestEngine::isRunning() const
{
    return m_running;
}

bool IngestEngine::ensureBox(QString *error) const
{
    if (m_boxPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("收件箱路径为空");
        return false;
    }
    if (QDir().mkpath(m_boxPath))
        return true;
    if (error)
        *error = QStringLiteral("无法创建收件箱目录：") + m_boxPath;
    return false;
}

bool IngestEngine::start()
{
    if (m_running)
        return true;

    QString error;
    if (!ensureBox(&error)) {
        setNotice(error);
        Logger::get().error("IngestEngine: {}", error.toStdString());
        return false;
    }
    if (!m_db || !m_db->isOpen()) {
        setNotice(QStringLiteral("下载数据库未打开，接管不可用"));
        return false;
    }

    reconcile();

    // 记录源状态（sources 表：Phase D 的 IM 适配器会共用同一张表）
    if (m_source) {
        QJsonArray dirs;
        for (const IDownloadSource::WatchRoot &root : m_source->roots())
            dirs.append(root.dir);

        DownloadsDatabase::SourceRow sourceRow;
        sourceRow.sourceId = m_source->sourceId();
        sourceRow.enabled = m_enabled;
        sourceRow.pathsJson = QString::fromUtf8(QJsonDocument(dirs).toJson(QJsonDocument::Compact));
        sourceRow.lastSeen = nowIso();
        m_db->upsertSource(sourceRow);
    }

    m_timer->start();
    m_running = true;
    return true;
}

void IngestEngine::stop()
{
    if (m_timer)
        m_timer->stop();
    m_running = false;
}

int IngestEngine::reconcile()
{
    if (!m_db || !m_db->isOpen())
        return 0;

    QStringList notes;
    reconcileJournal(&notes);
    if (m_autoReconcileBox)
        reconcileBox(&notes);

    if (!notes.isEmpty()) {
        const QString text = QStringLiteral("启动对账：") + notes.join(QStringLiteral("；"));
        setNotice(text);
        Logger::get().info("IngestEngine: {}", text.toStdString());
        emit changed();
    }
    return notes.size();
}

void IngestEngine::reconcileJournal(QStringList *notes)
{
    const QVector<JournalRow> intents = m_db->pendingIntents();
    for (const JournalRow &j : intents) {
        const auto row = m_db->byId(j.downloadId);
        if (!row) {
            m_db->finishJournal(j.id, false, 0);
            continue;
        }

        if (j.op == QLatin1String("fetch")) {
            const QFileInfo target(j.toPath);
            if (target.isFile() && target.size() == row->size) {
                registerCaptured(row->id, j.toPath);
                m_db->finishJournal(j.id, true);
                notes->append(QStringLiteral("补完拉取 %1").arg(row->name));
            } else {
                m_db->finishJournal(j.id, false, 0);
                m_db->updatePipeState(row->id, QStringLiteral("Failed"), 0);
                notes->append(QStringLiteral("拉取中断 %1（源文件保留）").arg(row->name));
            }
        } else if (j.op == QLatin1String("undo")) {
            if (QFileInfo::exists(j.toPath)) {
                m_db->updatePipeState(row->id, QStringLiteral("Undone"), 0);
                m_db->updateTrackState(row->id, QStringLiteral("Unknown"), j.toPath, false, nowIso());
                m_db->updatePaths(row->id, j.toPath, j.toPath);
                m_db->finishJournal(j.id, true);
                notes->append(QStringLiteral("补完撤销 %1").arg(row->name));
            } else {
                m_db->finishJournal(j.id, false, 0);
                notes->append(QStringLiteral("撤销中断 %1").arg(row->name));
            }
        } else {
            const bool ok = QFileInfo::exists(j.toPath);
            m_db->finishJournal(j.id, ok, 0);
            notes->append(QStringLiteral("补账 %1 %2").arg(j.op, row->name));
        }
    }
}

void IngestEngine::reconcileBox(QStringList *notes)
{
    const QVector<DownloadRow> rows = m_db->listInBox();
    int ghosts = 0;
    for (const DownloadRow &row : rows) {
        if (row.boxPath.isEmpty() || QFileInfo::exists(row.boxPath))
            continue;
        // Phase C 没有去向追踪：只能如实标 Unknown，不猜位置、不判"已删除"
        m_db->updateTrackState(row.id, QStringLiteral("Unknown"), row.boxPath, false, nowIso());
        m_db->updatePipeState(row.id, QStringLiteral("InBox"), 0);
        ++ghosts;
    }
    if (ghosts > 0) {
        notes->append(QStringLiteral("%1 个文件已离开收件箱（位置待 Phase D 追踪）").arg(ghosts));
    }
}

void IngestEngine::handleBatch(const QVector<FileSystemEvent> &batch)
{
    if (!m_enabled || !m_source)
        return;

    for (const FileSystemEvent &ev : batch) {
        switch (ev.type) {
        case FileSystemEvent::Overflow:
            if (isUnderSource(ev.path))
                m_candidates.clear();
            break;
        case FileSystemEvent::Removed:
            removeCandidate(ev.path);
            break;
        case FileSystemEvent::Renamed:
            removeCandidate(ev.oldPath);
            considerPath(ev.path);
            break;
        case FileSystemEvent::Added:
        case FileSystemEvent::Modified:
            considerPath(ev.path);
            break;
        }
    }
}

bool IngestEngine::isUnderSource(const QString &path) const
{
    if (!m_source)
        return false;
    const QString p = QDir::cleanPath(path).toLower();
    for (const IDownloadSource::WatchRoot &root : m_source->roots()) {
        QString r = QDir::cleanPath(root.dir).toLower();
        while (r.size() > 3 && r.endsWith(QLatin1Char('/')))
            r.chop(1);
        if (r.isEmpty())
            continue;
        if (p == r || p.startsWith(r + QLatin1Char('/')))
            return true;
    }
    return false;
}

bool IngestEngine::hasPartialExtension(const QString &name) const
{
    if (!m_source)
        return false;
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot < 0 || dot + 1 >= name.size())
        return false;
    const QString ext = name.mid(dot + 1).toLower();
    for (const QString &partial : m_source->partialExtensions()) {
        if (ext == partial.toLower())
            return true;
    }
    return false;
}

int IngestEngine::effectiveSettleMs() const
{
    if (m_settleOverrideMs > 0)
        return m_settleOverrideMs;
    return m_source ? m_source->settleMs() : 1200;
}

void IngestEngine::considerPath(const QString &path)
{
    if (path.isEmpty() || !isUnderSource(path))
        return;

    const QFileInfo info(path);
    if (!info.isFile())
        return;

    if (hasPartialExtension(info.fileName())) {
        removeCandidate(path);
        return;
    }

    const QString key = QDir::cleanPath(path).toLower();
    if (m_candidates.contains(key))
        return;

    if (m_db && m_db->bySrcPath(path))
        return;   // 已接管过（或失败过）：不重复登记

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Candidate c;
    c.path = QDir::fromNativeSeparators(path);
    c.size = info.size();
    c.mtime = info.lastModified().toSecsSinceEpoch();
    c.firstSeenMs = now;
    c.lastChangeMs = now;
    m_candidates.insert(key, c);

    Logger::get().debug("IngestEngine: 候选 {}", c.path.toStdString());
}

void IngestEngine::removeCandidate(const QString &path)
{
    m_candidates.remove(QDir::cleanPath(path).toLower());
}

void IngestEngine::tick()
{
    if (!m_enabled || m_candidates.isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 settle = effectiveSettleMs();

    QList<QString> drop;
    for (auto it = m_candidates.begin(); it != m_candidates.end(); ++it) {
        Candidate &c = it.value();
        const QFileInfo info(c.path);

        if (!info.isFile() || hasPartialExtension(info.fileName())) {
            drop.append(it.key());
            continue;
        }

        const qint64 size = info.size();
        const qint64 mtime = info.lastModified().toSecsSinceEpoch();
        if (size != c.size || mtime != c.mtime) {
            c.size = size;
            c.mtime = mtime;
            c.lastChangeMs = now;
            continue;   // 还在写：重新计时
        }

        if (now - c.lastChangeMs < settle)
            continue;
        if (c.nextTryMs > now)
            continue;

        if (!FileIdentity::canOpenExclusive(c.path)) {
            if (c.attempts < kMaxExclusiveAttempts) {
                c.nextTryMs = now + kBackoffMs[c.attempts];
                ++c.attempts;
                Logger::get().debug("IngestEngine: {} 被占用，{} ms 后重试（第 {} 次）",
                                    c.path.toStdString(), kBackoffMs[c.attempts - 1], c.attempts);
                continue;
            }
            // C 重试用尽：以 A+B 结论为准，显式记录（§2.3）
            c.abOnly = true;
        }

        admit(c);
        drop.append(it.key());
    }

    for (const QString &key : drop)
        m_candidates.remove(key);
}

QString IngestEngine::uniqueTarget(const QString &dir, const QString &fileName) const
{
    const QString direct = dir + QLatin1Char('/') + fileName;
    if (!QFileInfo::exists(direct))
        return direct;

    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.completeSuffix();
    for (int i = 1; i < 10000; ++i) {
        const QString candidate = dir + QLatin1Char('/') + base
                                  + QStringLiteral(" (%1)").arg(i)
                                  + (suffix.isEmpty() ? QString()
                                                      : QStringLiteral(".") + suffix);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return direct;
}

void IngestEngine::admit(const Candidate &candidate)
{
    if (!m_db || !m_db->isOpen())
        return;

    QString error;
    if (!ensureBox(&error)) {
        setNotice(error);
        return;
    }

    const QFileInfo srcInfo(candidate.path);
    if (!srcInfo.isFile())
        return;

    const QString fileName = srcInfo.fileName();
    const QString target = uniqueTarget(m_boxPath, fileName);

    if (!hasEnoughSpace(candidate.path, m_boxPath, srcInfo.size())) {
        setNotice(QStringLiteral("磁盘空间不足，未接管 %1（源文件保留）").arg(fileName));
        Logger::get().warn("IngestEngine: 空间不足，跳过 {}", candidate.path.toStdString());
        return;
    }

    DownloadRow row;
    row.sourceId = m_source ? m_source->sourceId() : QStringLiteral("browser");
    row.srcPath = candidate.path;
    row.boxPath = target;
    row.curPath = target;
    row.inBox = false;
    row.name = fileName;
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    row.ext = dot > 0 ? fileName.mid(dot + 1).toLower() : QString();
    {
        static const FileClassifier classifier;   // 内置表只读，构造一次
        row.catMask = classifier.classify(fileName);
    }
    row.size = srcInfo.size();
    row.mtime = srcInfo.lastModified().toSecsSinceEpoch();
    row.pipeState = QStringLiteral("Fetching");
    row.trackState = QStringLiteral("InBox");
    row.detectedBy = QStringLiteral("watcher");
    row.createdAt = nowIso();

    // 规则 can 让某个文件**完全不接管**（如“安装包别动”）
    if (m_rules.count() > 0) {
        const DownloadRules::Decision pre = m_rules.evaluate(row, m_boxPath);
        if (pre.action == QLatin1String("skip")) {
            Logger::get().info("IngestEngine: 规则「{}」跳过 {}",
                               pre.ruleName.toStdString(), fileName.toStdString());
            return;
        }
    }

    const int id = m_db->insert(row);
    if (id < 0) {
        setNotice(QStringLiteral("登记下载失败：") + m_db->lastError());
        return;
    }

    const qint64 journal = m_db->beginJournal(id, QStringLiteral("fetch"), candidate.path, target);

    DWORD moveError = 0;
    if (!moveFile(candidate.path, target, &moveError)) {
        if (journal > 0)
            m_db->finishJournal(journal, false, static_cast<int>(moveError));
        m_db->updatePipeState(id, QStringLiteral("Failed"), static_cast<int>(moveError));
        const QString msg = QStringLiteral("拉取失败（%1）：%2 — 源文件保留")
                                .arg(fileName).arg(moveError);
        setNotice(msg);
        Logger::get().warn("IngestEngine: {}（Win32 {}）",
                           msg.toStdString(), static_cast<unsigned>(moveError));
        emit changed();
        return;
    }

    registerCaptured(id, target);
    if (journal > 0)
        m_db->finishJournal(journal, true);

    // 归档规则：默认只写「建议去向」；autoOrganize 开启时 move 才执行
    if (m_rules.count() > 0) {
        const auto stored = m_db->byId(id);
        if (stored) {
            const DownloadRules::Decision decision = m_rules.evaluate(*stored, m_boxPath);
            if (decision.matched()) {
                m_db->setSuggestion(id, decision.dest, decision.ruleName);
                if (decision.action == QLatin1String("move")
                    && m_autoOrganize && !decision.dest.isEmpty()) {
                    QString archiveError;
                    if (!archive(id, decision.dest, &archiveError))
                        Logger::get().warn("IngestEngine: 自动归档失败 — {}",
                                           archiveError.toStdString());
                }
            }
        }
    }

    const qint64 size = QFileInfo(target).size();
    Logger::get().info("IngestEngine: 已接管 {} → {}（{} 字节）",
                       candidate.path.toStdString(), target.toStdString(), size);
    emit captured(id, fileName, size);
    emit changed();
}

void IngestEngine::registerCaptured(int id, const QString &path)
{
    FileIdentity::Identity identity;
    const bool identityOk = FileIdentity::read(path, &identity);
    const QString sig = FileIdentity::quickSignature(path);
    const QFileInfo info(path);

    m_db->updateAfterFetch(id,
                           path,
                           info.size(),
                           info.lastModified().toSecsSinceEpoch(),
                           identityOk ? identity.volumeSerial : 0,
                           identityOk ? identity.fileId : QString(),
                           sig);

    // 查重：同 quickSig 的更早记录 → dup_of（裁决仍由用户决定，这里只标记）
    if (!sig.isEmpty()) {
        const auto duplicate = m_db->bySigQuick(sig, id);
        if (duplicate)
            m_db->setDupOf(id, duplicate->id);
    }
}

bool IngestEngine::archive(int downloadId, const QString &destDir, QString *error)
{
    if (!m_db || !m_db->isOpen()) {
        if (error)
            *error = QStringLiteral("下载数据库未打开");
        return false;
    }
    const auto row = m_db->byId(downloadId);
    if (!row || !row->inBox || row->boxPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("该记录不在收件箱，无法归档");
        return false;
    }
    if (destDir.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("归档目录为空");
        return false;
    }

    const QString dir = QDir::fromNativeSeparators(destDir);
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("无法创建归档目录：") + dir;
        return false;
    }
    const QString target = uniqueTarget(dir, row->name);

    // 不覆盖已存在的目标：uniqueTarget 已保证；这里再防御一次
    if (QFileInfo::exists(target)) {
        if (error)
            *error = QStringLiteral("目标已存在：") + target;
        return false;
    }

    const qint64 journal = m_db->beginJournal(downloadId, QStringLiteral("organize"),
                                              row->boxPath, target);
    DWORD moveError = 0;
    if (!moveFile(row->boxPath, target, &moveError)) {
        if (journal > 0)
            m_db->finishJournal(journal, false, static_cast<int>(moveError));
        if (error)
            *error = QStringLiteral("归档移动失败（Win32 %1）").arg(moveError);
        return false;
    }

    if (journal > 0)
        m_db->finishJournal(journal, true);
    m_db->updateAfterArchive(downloadId, target);

    Logger::get().info("IngestEngine: 已归档 {} → {}",
                       row->boxPath.toStdString(), target.toStdString());
    setNotice(QStringLiteral("已归档 %1 → %2").arg(row->name, QDir::toNativeSeparators(dir)));
    emit changed();
    return true;
}

bool IngestEngine::unarchive(int downloadId, QString *error)
{
    if (!m_db || !m_db->isOpen()) {
        if (error)
            *error = QStringLiteral("下载数据库未打开");
        return false;
    }
    const auto row = m_db->byId(downloadId);
    if (!row || row->inBox) {
        if (error)
            *error = QStringLiteral("该记录仍在收件箱，无需撤销归档");
        return false;
    }

    // 只撤销**本应用**做的归档；用户自己移走的必须由 TrackingEngine 处理
    JournalRow organize;
    bool found = false;
    const QVector<JournalRow> journals = m_db->journalFor(downloadId);
    for (const JournalRow &journal : journals) {
        if (journal.op == QLatin1String("organize")
            && journal.opState == QLatin1String("done")
            && journal.toPath == row->curPath) {
            organize = journal;
            found = true;
        }
    }
    if (!found) {
        if (error)
            *error = QStringLiteral("这条不是本应用归档的（文件可能被手动移动），不能撤销归档");
        return false;
    }
    if (!QFileInfo::exists(row->curPath)) {
        if (error)
            *error = QStringLiteral("归档后的文件已不存在：") + row->curPath;
        return false;
    }

    QDir().mkpath(m_boxPath);
    const QString target = uniqueTarget(m_boxPath, row->name);
    const qint64 journal = m_db->beginJournal(downloadId, QStringLiteral("unarchive"),
                                              row->curPath, target);
    DWORD moveError = 0;
    if (!moveFile(row->curPath, target, &moveError)) {
        if (journal > 0)
            m_db->finishJournal(journal, false, static_cast<int>(moveError));
        if (error)
            *error = QStringLiteral("撤销归档失败（Win32 %1）").arg(moveError);
        return false;
    }

    if (journal > 0)
        m_db->finishJournal(journal, true);
    m_db->updateAfterUnarchive(downloadId, target);

    Logger::get().info("IngestEngine: 撤销归档 {} → {}",
                       organize.toPath.toStdString(), target.toStdString());
    setNotice(QStringLiteral("已撤销归档：%1").arg(row->name));
    emit changed();
    return true;
}

bool IngestEngine::undo(int downloadId, QString *error)
{
    if (!m_db || !m_db->isOpen()) {
        if (error)
            *error = QStringLiteral("下载数据库未打开");
        return false;
    }

    const auto row = m_db->byId(downloadId);
    if (!row || !row->inBox || row->boxPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("该记录不在收件箱，无法撤销");
        return false;
    }

    const QString sourceDir = QFileInfo(row->srcPath).absolutePath().isEmpty()
                                  ? m_boxPath
                                  : QFileInfo(row->srcPath).absolutePath();
    QDir().mkpath(sourceDir);
    const QString target = uniqueTarget(sourceDir, row->name);

    const qint64 journal = m_db->beginJournal(downloadId, QStringLiteral("undo"),
                                              row->boxPath, target);
    DWORD moveError = 0;
    if (!moveFile(row->boxPath, target, &moveError)) {
        if (journal > 0)
            m_db->finishJournal(journal, false, static_cast<int>(moveError));
        if (error)
            *error = QStringLiteral("撤销失败（Win32 %1）").arg(moveError);
        return false;
    }

    if (journal > 0)
        m_db->finishJournal(journal, true);
    m_db->updatePipeState(downloadId, QStringLiteral("Undone"), 0);
    m_db->updateTrackState(downloadId, QStringLiteral("Unknown"), target, false, nowIso());
    m_db->updatePaths(downloadId, target, target);

    Logger::get().info("IngestEngine: 撤销接管 {} → {}",
                       row->boxPath.toStdString(), target.toStdString());
    emit changed();
    return true;
}

void IngestEngine::setNotice(const QString &text)
{
    m_lastNotice = text;
    emit notice(text);
}
