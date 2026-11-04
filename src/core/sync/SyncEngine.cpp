#include "core/sync/SyncEngine.h"
#include "core/sync/PathGuard.h"
#include "database/SyncDatabase.h"
#include "core/Logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>

namespace {

QString hashFile(const QString &absPath)
{
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        return QString();
    return QString::fromLatin1(h.result().toHex());
}

/// "a/b/c.txt" → "a/b/"（无目录时返回空串）
QString dirPrefixOf(const QString &relPath)
{
    const int idx = relPath.lastIndexOf(QLatin1Char('/'));
    return idx < 0 ? QString() : relPath.left(idx + 1);
}

/// 别名必须是合法的**单层**目录名：能直接当路径一段用，又不能是 "../" 之类。
bool aliasOk(const QString &alias)
{
    return !alias.isEmpty()
        && !alias.contains(QLatin1Char('/'))
        && !alias.contains(QLatin1Char('\\'))
        && Sync::PathGuard::isSafeRelativePath(alias);
}

/// 递归上限：即使 junction 漏判也不至于跑成无限深。
constexpr int kMaxScanDepth = 64;

/// 深度优先收集一个目录下的文件。
///
/// **为什么不用 QDirIterator**：它在 Windows 上不认 junction —— Qt 把 junction 当普通
/// 目录（`isSymLink()` 返回 false），`Subdirectories` 会一路降下去。本地共享根里若有一个
/// 指回上层的 junction，扫描就会无限套娃（项目已在 FileSearcher 上踩过同一个坑，
/// 见 docs/troubleshooting/qt-nosymlinks-挡不住junction.md）。
/// 因此与 FileSearcher::walkDir 保持一致的写法：自己控制是否下降。
void scanDirInto(const QString &root, const QString &dirPath, int depth,
                 QHash<QString, Sync::FileState> &out)
{
    if (depth > kMaxScanDepth)
        return;

    const QFileInfoList entries =
        QDir(dirPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                    QDir::NoSort);
    for (const QFileInfo &info : entries) {
        if (info.isDir()) {
            if (info.isSymLink() || info.isJunction())
                continue;                                    // 不跟随 reparse point
            if (info.fileName() == QLatin1String(".sync"))
                continue;                                    // 同步元数据不参与同步
            scanDirInto(root, info.filePath(), depth + 1, out);
            continue;
        }
        if (info.isSymLink())
            continue;

        QString rel = QDir(root).relativeFilePath(info.filePath());
        rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (rel.startsWith(QStringLiteral("..")))
            continue;

        Sync::FileState s;
        s.size  = info.size();
        s.mtime = info.lastModified().toSecsSinceEpoch();
        s.hash  = hashFile(info.filePath());
        out.insert(rel, s);
    }
}

} // namespace

namespace Sync {

SyncEngine::SyncEngine(QObject *parent)
    : QObject(parent)
{
}

void SyncEngine::setDatabase(SyncDatabase *db)            { m_db = db; }
void SyncEngine::setDeviceName(const QString &name)       { m_deviceName = name; }
void SyncEngine::setTransports(const QList<std::shared_ptr<ITransport>> &list) { m_transports = list; }

void SyncEngine::setRoots(const QList<SyncRoot> &roots)
{
    m_roots.clear();
    QSet<QString> usedAlias;
    for (const SyncRoot &r : roots) {
        if (r.localPath.trimmed().isEmpty() || !aliasOk(r.alias))
            continue;
        if (usedAlias.contains(r.alias))
            continue;                       // 别名重复的后来者忽略，不覆盖先来的
        usedAlias.insert(r.alias);
        m_roots.append(SyncRoot{ QDir(r.localPath).absolutePath(), r.alias });
    }
}

void SyncEngine::setLocalRoot(const QString &root)
{
    const QString abs = QDir(root).absolutePath();
    setRoots({ SyncRoot{ abs, QFileInfo(abs).fileName() } });
}

QString SyncEngine::rootOfAlias(const QString &alias) const
{
    for (const SyncRoot &r : m_roots) {
        if (r.alias == alias)
            return r.localPath;
    }
    return QString();
}

QString SyncEngine::localTrashDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/Assistant");
    return dir + QStringLiteral("/sync-trash");
}

QHash<QString, FileState> SyncEngine::scanLocal() const
{
    QHash<QString, FileState> out;

    for (const SyncRoot &r : m_roots) {
        if (!QDir(r.localPath).exists())
            continue;                        // 单个文件夹暂时不在（盘没插）不拖垮其他

        QHash<QString, FileState> one;
        scanDirInto(r.localPath, r.localPath, 0, one);

        const QString prefix = r.alias + QLatin1Char('/');
        for (auto it = one.constBegin(); it != one.constEnd(); ++it)
            out.insert(prefix + it.key(), it.value());
    }
    return out;
}

QString SyncEngine::localAbsFor(const QString &relPath) const
{
    const int slash = relPath.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QString();

    const QString root  = rootOfAlias(relPath.left(slash));
    const QString inner = relPath.mid(slash + 1);
    if (root.isEmpty() || inner.isEmpty() || !PathGuard::isSafeRelativePath(inner))
        return QString();

    const QString abs = QDir(root).absoluteFilePath(inner);
    if (!QDir().mkpath(QFileInfo(abs).absolutePath()))
        return QString();

    return PathGuard::resolveUnder(root, inner);
}

bool SyncEngine::moveToLocalTrash(const QString &absPath) const
{
    const QString dir = localTrashDir();
    if (dir.isEmpty() || !QDir().mkpath(dir))
        return false;

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    const QString dest  = dir + QLatin1Char('/') + stamp + QLatin1Char('-') + QFileInfo(absPath).fileName();

    QFile::remove(dest);
    if (QFile::rename(absPath, dest))
        return true;
    if (QFile::copy(absPath, dest))
        return QFile::remove(absPath);
    return false;
}

SyncResult SyncEngine::sync()
{
    SyncResult result;

    if (!m_db || !m_db->isOpen()) {
        result.status  = SyncResult::Aborted;
        result.message = QStringLiteral("同步数据库不可用");
        return result;
    }
    if (m_roots.isEmpty()) {
        result.status  = SyncResult::Aborted;
        result.message = QStringLiteral("没有配置要同步的文件夹");
        return result;
    }
    {
        bool anyExists = false;
        for (const SyncRoot &r : m_roots) {
            if (QDir(r.localPath).exists()) {
                anyExists = true;
                break;
            }
        }
        if (!anyExists) {
            result.status  = SyncResult::Aborted;
            result.message = QStringLiteral("本地同步文件夹都不存在：") + m_roots.first().localPath;
            return result;
        }
    }

    // 1. 按优先级选通道：直连优先，跑不通自动降级到共享目录
    QList<std::shared_ptr<ITransport>> candidates = m_transports;
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const std::shared_ptr<ITransport> &a, const std::shared_ptr<ITransport> &b) {
                         return a->priority() < b->priority();
                     });

    std::shared_ptr<ITransport> channel;
    for (const std::shared_ptr<ITransport> &cand : candidates) {
        if (cand->probe()) {
            channel = cand;
            break;
        }
    }
    if (!channel) {
        result.status  = SyncResult::NoChannelAvailable;
        result.message = QStringLiteral("没有可用通道：请检查共享目录是否存在且可写");
        return result;
    }

    // 跨设备互斥：共享目录清单是"读-改-写"，两台机器同时跑会互相覆盖条目。
    // 拿不到锁就交还给上层，由定时器/去抖自动重试。
    if (!channel->tryLock(5000)) {
        result.status  = SyncResult::Busy;
        result.message = QStringLiteral("另一台设备正在同步，稍后自动重试");
        return result;
    }
    struct UnlockGuard
    {
        ITransport *t;
        ~UnlockGuard() { t->unlock(); }
    } guard{ channel.get() };
    (void)guard;

    // 2. 拉对端清单
    const QList<RemoteFileInfo> remoteList = channel->listRemote();
    const QList<QString> baselinePaths = m_db->allBaselinePaths();

    if (remoteList.isEmpty() && !baselinePaths.isEmpty()) {
        // 安全阀：清单为空但本地有基线。正常含义是"对端删光了"，但更常见的原因是
        // 共享目录刚被建出来/网盘把清单抓走了半个。此时照常判定会把本地所有文件
        // 判成"对端已删除"而清空 —— 宁可这一轮什么都不做。
        result.status  = SyncResult::RemoteUnreachable;
        result.message = QStringLiteral("共享目录里没有任何文件，而本机还留着上次同步的基线，"
                                        "本轮已跳过（避免误删本地文件）。"
                                        "若共享目录确实被清空了，请删掉 sync.db 后重启再同步。");
        return result;
    }

    auto statesOf = [](const QList<RemoteFileInfo> &list) {
        QHash<QString, FileState> m;
        for (const RemoteFileInfo &f : list) {
            FileState s;
            s.size  = f.size;
            s.mtime = f.mtime;
            s.hash  = f.hash;
            m.insert(f.relPath, s);
        }
        return m;
    };

    const QHash<QString, FileState> remote = statesOf(remoteList);

    // 改动对端之前会重新核对的"最新对端状态"（compare-and-swap），见循环内注释。
    QHash<QString, FileState> freshRemote;
    bool freshValid  = false;   // freshRemote 是否是本轮可用的最新读数
    bool casDisabled = false;   // 最新读数不可信时彻底关掉这个判断

    // 3. 扫描本地
    const QHash<QString, FileState> local = scanLocal();

    // 4. 三方并集
    QSet<QString> all;
    for (auto it = local.constBegin(); it != local.constEnd(); ++it)
        all.insert(it.key());
    for (auto it = remote.constBegin(); it != remote.constEnd(); ++it)
        all.insert(it.key());
    for (const QString &p : baselinePaths)
        all.insert(p);

    QStringList paths(all.constBegin(), all.constEnd());
    paths.sort();

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const int total = paths.size();
    int done = 0;

    for (const QString &relPath : paths) {
        emit progress(total > 0 ? done * 100 / total : 100, relPath);
        ++done;

        // 共享目录里可能有别的设备在同步、本机没参与的子目录。那不是错误，
        // 本机没有对应的本地文件夹自然无处落地，直接略过、不计入 failed。
        const QString aliasOfPath = relPath.left(relPath.indexOf(QLatin1Char('/')));
        if (relPath.indexOf(QLatin1Char('/')) <= 0 || rootOfAlias(aliasOfPath).isEmpty())
            continue;

        const FileState L = local.value(relPath, FileState{});
        FileState S;
        if (const std::optional<FileState> b = m_db->baseline(relPath))
            S = *b;
        FileState R = remote.value(relPath, FileState{});

        SyncAction action = decide(L, S, R);

        // 改动对端之前先核对一次：如果另一端在我们拉清单之后已经改掉了同一个文件，
        // 直接 push 会把它覆盖掉（后写覆盖先写，两边都不留副本 = 静默丢数据）。
        // 这里做一次 compare-and-swap：读数变了就用最新状态重新判定，通常会升级为冲突。
        if (action == SyncAction::Upload || action == SyncAction::DeleteRemote) {
            if (!freshValid && !casDisabled) {
                const QHash<QString, FileState> reread = statesOf(channel->listRemote());
                if (reread.isEmpty() && !remote.isEmpty()) {
                    casDisabled = true;    // 与上面的安全阀同理：清单读没了就不做判断
                } else {
                    freshRemote = reread;
                    freshValid  = true;
                }
            }
            if (freshValid) {
                const FileState Rnow = freshRemote.value(relPath, FileState{});
                if (!sameState(Rnow, R)) {
                    R      = Rnow;
                    action = decide(L, S, R);
                }
            }
        }

        switch (action) {
        case SyncAction::None:
            break;

        case SyncAction::Upload: {
            const QString abs = localAbsFor(relPath);
            if (!abs.isEmpty() && channel->push(abs, relPath)) {
                freshValid = false;
                m_db->upsertBaseline(relPath, L, now, QStringLiteral("push"));
                ++result.uploaded;
                result.bytesMoved += qMax<qint64>(L.size, 0);
                emit fileHandled(relPath, QStringLiteral("upload"));
            } else {
                ++result.failed;
            }
            break;
        }

        case SyncAction::Download: {
            const QString dest = localAbsFor(relPath);
            if (!dest.isEmpty() && channel->fetch(relPath, dest)) {
                const QFileInfo fi(dest);
                FileState got = R;
                got.size  = fi.size();
                got.mtime = fi.lastModified().toSecsSinceEpoch();
                m_db->upsertBaseline(relPath, got, now, QStringLiteral("pull"));
                ++result.downloaded;
                result.bytesMoved += qMax<qint64>(got.size, 0);
                emit fileHandled(relPath, QStringLiteral("download"));
            } else {
                ++result.failed;
            }
            break;
        }

        case SyncAction::UpdateBaseline: {
            if (L.exists())
                m_db->upsertBaseline(relPath, L, now, QStringLiteral("push"));
            else
                m_db->removeBaseline(relPath);
            emit fileHandled(relPath, QStringLiteral("skip"));
            break;
        }

        case SyncAction::DeleteLocal: {
            const QString abs = localAbsFor(relPath);
            if (abs.isEmpty()) {
                ++result.failed;
                break;
            }
            if (QFileInfo::exists(abs) && !moveToLocalTrash(abs)) {
                ++result.failed;
                break;
            }
            m_db->removeBaseline(relPath);
            ++result.deleted;
            emit fileHandled(relPath, QStringLiteral("delete"));
            break;
        }

        case SyncAction::DeleteRemote: {
            if (channel->removeRemote(relPath)) {
                freshValid = false;
                m_db->removeBaseline(relPath);
                ++result.deleted;
                emit fileHandled(relPath, QStringLiteral("delete"));
            } else {
                ++result.failed;
            }
            break;
        }

        case SyncAction::Conflict: {
            ++result.conflicts;

            const QString copyRel = dirPrefixOf(relPath)
                                    + makeConflictName(QFileInfo(relPath).fileName(), m_deviceName, now);
            const QString localAbs = localAbsFor(relPath);
            const QString copyAbs  = localAbsFor(copyRel);

            // 5a. "删除 vs 修改"不算真冲突：修改胜出，绝不丢内容
            if (L.exists() && !R.exists()) {
                if (channel->push(localAbs, relPath)) {
                    freshValid = false;
                    m_db->upsertBaseline(relPath, L, now, QStringLiteral("push"));
                    ++result.uploaded;
                    emit fileHandled(relPath, QStringLiteral("conflict"));
                } else {
                    ++result.failed;
                }
                break;
            }
            if (!L.exists() && R.exists()) {
                const QString dest = localAbsFor(relPath);
                if (!dest.isEmpty() && channel->fetch(relPath, dest)) {
                    m_db->upsertBaseline(relPath, R, now, QStringLiteral("pull"));
                    ++result.downloaded;
                    emit fileHandled(relPath, QStringLiteral("conflict"));
                } else {
                    ++result.failed;
                }
                break;
            }

            // 5b. 两端都改了不同内容。胜者必须由两台机器算出同一个结果，否则会互相覆盖
            //     来回拉锯 —— 因此只依赖双方都有的 (mtime, hash)，不用"本地优先"。
            const bool localWins = (L.mtime != R.mtime)
                                       ? (L.mtime > R.mtime)
                                       : (QString::compare(L.hash, R.hash, Qt::CaseSensitive) > 0);

            bool ok = false;
            if (copyAbs.isEmpty()) {
                ++result.failed;
                break;
            }

            if (localWins) {
                // 先把对端的内容取到副本里，再把本地内容推成共享目录的当前值；
                // 少了后一步，下一轮会判成"对端又改了"把胜者覆盖掉。
                if (channel->fetch(relPath, copyAbs) && channel->push(localAbs, relPath)) {
                    freshValid = false;
                    m_db->upsertBaseline(relPath, L, now, QStringLiteral("push"));
                    ++result.uploaded;
                    ok = true;
                }
            } else {
                QFile::remove(copyAbs);
                if (QFile::copy(localAbs, copyAbs) && channel->fetch(relPath, localAbs)) {
                    m_db->upsertBaseline(relPath, R, now, QStringLiteral("pull"));
                    ++result.downloaded;
                    ok = true;
                }
            }

            if (ok) {
                ConflictRecord rec;
                rec.relPath      = relPath;
                rec.localHash    = L.hash;
                rec.remoteHash   = R.hash;
                rec.conflictCopy = copyRel;
                rec.detectedAt   = now;
                m_db->insertConflict(rec);
                emit fileHandled(relPath, QStringLiteral("conflict"));
            } else {
                ++result.failed;
            }
            break;
        }
        }
    }

    emit progress(100, QString());
    Logger::get().info("SyncEngine: 通道={} 上传={} 下载={} 冲突={} 删除={} 失败={}",
                       channel->name().toStdString(), result.uploaded, result.downloaded,
                       result.conflicts, result.deleted, result.failed);
    return result;
}

} // namespace Sync
