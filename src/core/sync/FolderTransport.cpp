#include "core/sync/FolderTransport.h"
#include "core/sync/PathGuard.h"
#include "core/Logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

/// 锁文件超过这么久没更新，视为进程崩溃留下的残留锁，可被接管
constexpr qint64 kStaleLockSeconds = 30;

} // namespace

namespace Sync {

FolderTransport::FolderTransport(const QString &sharedDir)
{
    if (!sharedDir.isEmpty())
        m_sharedDir = QDir(QDir::fromNativeSeparators(sharedDir)).absolutePath();
}

FolderTransport::~FolderTransport()
{
    unlock();
}

QString FolderTransport::name() const
{
    return QStringLiteral("共享目录");
}

int FolderTransport::priority() const
{
    return 10;
}

QString FolderTransport::metaDir() const
{
    return m_sharedDir + QStringLiteral("/.sync");
}

QString FolderTransport::manifestPath() const
{
    return metaDir() + QStringLiteral("/manifest.json");
}

QString FolderTransport::lockPath() const
{
    return metaDir() + QStringLiteral("/lock.json");
}

QString FolderTransport::trashDir() const
{
    return metaDir() + QStringLiteral("/trash");
}

bool FolderTransport::hasManifest() const
{
    return QFileInfo::exists(manifestPath());
}

bool FolderTransport::probe()
{
    if (m_sharedDir.isEmpty())
        return false;
    if (!QDir(m_sharedDir).exists())
        return false;
    if (!QDir().mkpath(metaDir()))
        return false;

    // 真的写一次：网盘/只读目录可能"存在但不可写"
    const QString probeFile = metaDir() + QStringLiteral("/.write-probe");
    QFile f(probeFile);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write("ok");
    f.close();
    QFile::remove(probeFile);
    return true;
}

QString FolderTransport::sharedPathFor(const QString &relPath) const
{
    if (m_sharedDir.isEmpty() || !PathGuard::isSafeRelativePath(relPath))
        return QString();

    const QString normalized = QString(relPath).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString abs = QDir(m_sharedDir).absoluteFilePath(normalized);

    // resolveUnder 要求父目录可解析，先建出来
    if (!QDir().mkpath(QFileInfo(abs).absolutePath()))
        return QString();

    return PathGuard::resolveUnder(m_sharedDir, relPath);
}

QString FolderTransport::hashOf(const QString &absPath)
{
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        return QString();
    return QString::fromLatin1(h.result().toHex());
}

RemoteFileInfo FolderTransport::infoOf(const QString &relPath, const QString &absPath)
{
    RemoteFileInfo info;
    const QFileInfo fi(absPath);
    info.relPath = relPath;
    info.size    = fi.size();
    info.mtime   = fi.lastModified().toSecsSinceEpoch();
    info.hash    = hashOf(absPath);
    return info;
}

bool FolderTransport::copyAtomically(const QString &src, const QString &dest)
{
    const QString tmp = dest + QStringLiteral(".asynctmp");
    QFile::remove(tmp);
    if (!QFile::copy(src, tmp)) {
        QFile::remove(tmp);
        return false;
    }
    QFile::remove(dest);
    if (!QFile::rename(tmp, dest)) {
        QFile::remove(tmp);
        return false;
    }
    return true;
}

QHash<QString, RemoteFileInfo> FolderTransport::readManifest() const
{
    QHash<QString, RemoteFileInfo> out;

    m_manifestUpdatedAt = 0;

    QFile f(manifestPath());
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QByteArray raw = f.readAll();
    f.close();

    try {
        const json j = json::parse(raw.toStdString());
        m_remoteDevice = QString::fromStdString(j.value("device", std::string()));
        m_manifestUpdatedAt = static_cast<qint64>(j.value("updatedAt", static_cast<long long>(0)));
        if (j.contains("files") && j["files"].is_array()) {
            for (const auto &e : j["files"]) {
                RemoteFileInfo info;
                info.relPath = QString::fromStdString(e.value("relPath", std::string()));
                info.size    = static_cast<qint64>(e.value("size", static_cast<long long>(-1)));
                info.mtime   = static_cast<qint64>(e.value("mtime", static_cast<long long>(0)));
                info.hash    = QString::fromStdString(e.value("hash", std::string()));
                if (!info.relPath.isEmpty())
                    out.insert(info.relPath, info);
            }
        }
    } catch (const std::exception &e) {
        Logger::get().warn("FolderTransport: manifest 解析失败 — {}", e.what());
    }
    return out;
}

bool FolderTransport::writeManifest(const QHash<QString, RemoteFileInfo> &files) const
{
    json j;
    j["version"]   = 2;
    j["device"]    = m_deviceName.toStdString();
    j["updatedAt"] = QDateTime::currentSecsSinceEpoch();

    json arr = json::array();
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        json e;
        e["relPath"] = it.value().relPath.toStdString();
        e["size"]    = static_cast<long long>(it.value().size);
        e["mtime"]   = static_cast<long long>(it.value().mtime);
        e["hash"]    = it.value().hash.toStdString();
        arr.push_back(e);
    }
    j["files"] = arr;

    const std::string dumped = j.dump(2);
    const QString tmp = manifestPath() + QStringLiteral(".tmp");

    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::get().warn("FolderTransport: 无法写清单 {}", tmp.toStdString());
        return false;
    }
    f.write(dumped.c_str(), static_cast<qint64>(dumped.size()));
    f.close();

    // 原子替换：网盘可能随时抓走这个文件，半个 JSON 会让对端整轮同步失效
    QFile::remove(manifestPath());
    if (!QFile::rename(tmp, manifestPath())) {
        QFile::remove(tmp);
        Logger::get().warn("FolderTransport: 清单替换失败");
        return false;
    }
    return true;
}

bool FolderTransport::updateManifestEntry(const QString &relPath,
                                          const RemoteFileInfo &info,
                                          bool removeEntry)
{
    QHash<QString, RemoteFileInfo> files = readManifest();
    if (removeEntry)
        files.remove(relPath);
    else
        files.insert(relPath, info);
    return writeManifest(files);
}

QList<RemoteFileInfo> FolderTransport::listRemote()
{
    QList<RemoteFileInfo> out;
    const QHash<QString, RemoteFileInfo> files = readManifest();
    out.reserve(files.size());
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        // 清单条目以实际落盘为准，避免网盘改了 mtime 导致误判
        out.append(it.value());
    }
    return out;
}

bool FolderTransport::fetch(const QString &relPath, const QString &destPath)
{
    const QString src = sharedPathFor(relPath);
    if (src.isEmpty() || !QFileInfo::exists(src))
        return false;

    if (!QDir().mkpath(QFileInfo(destPath).absolutePath()))
        return false;

    return copyAtomically(src, destPath);
}

bool FolderTransport::push(const QString &localPath, const QString &relPath)
{
    if (!QFileInfo::exists(localPath))
        return false;

    const QString dest = sharedPathFor(relPath);
    if (dest.isEmpty()) {
        Logger::get().warn("FolderTransport: 拒绝推送不安全路径 {}", relPath.toStdString());
        return false;
    }

    if (!copyAtomically(localPath, dest))
        return false;

    return updateManifestEntry(relPath, infoOf(relPath, dest), false);
}

bool FolderTransport::removeRemote(const QString &relPath)
{
    const QString target = sharedPathFor(relPath);
    if (target.isEmpty())
        return false;

    bool ok = true;
    if (QFileInfo::exists(target)) {
        QDir().mkpath(trashDir());
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
        const QString trashPath = trashDir() + QLatin1Char('/') + stamp
                                  + QLatin1Char('-') + QFileInfo(target).fileName();
        QFile::remove(trashPath);
        if (!QFile::rename(target, trashPath)) {
            // 跨卷或被占用时退化为复制 + 删除
            if (QFile::copy(target, trashPath))
                QFile::remove(target);
            else
                ok = false;
        }
    }

    if (!updateManifestEntry(relPath, RemoteFileInfo{}, true))
        ok = false;

    return ok;
}

bool FolderTransport::tryLock(int waitMs)
{
    if (m_locked)
        return true;
    if (m_sharedDir.isEmpty())
        return false;
    if (!QDir().mkpath(metaDir()))
        return false;

    const QString path = lockPath();
    QElapsedTimer clock;
    clock.start();

    for (;;) {
        // NewOnly 在 Windows/SMB 上映射为 CREATE_NEW：不存在才创建，天然原子
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            const std::string body =
                json{ { "device", m_deviceName.toStdString() },
                      { "time", QDateTime::currentSecsSinceEpoch() } }.dump();
            f.write(body.c_str(), static_cast<qint64>(body.size()));
            f.close();
            m_locked = true;
            return true;
        }

        // 锁文件已存在：太旧视为崩溃残留，接管一次
        const QFileInfo fi(path);
        if (fi.exists() && QDateTime::currentSecsSinceEpoch()
                                   - fi.lastModified().toSecsSinceEpoch() > kStaleLockSeconds) {
            const QString stale = path + QStringLiteral(".stale");
            QFile::remove(stale);
            if (QFile::rename(path, stale)) {
                QFile::remove(stale);
                continue;
            }
        }

        if (waitMs <= 0 || clock.elapsed() >= waitMs)
            return false;
        QThread::msleep(300);
    }
}

void FolderTransport::unlock()
{
    if (!m_locked)
        return;
    m_locked = false;
    if (!m_sharedDir.isEmpty())
        QFile::remove(lockPath());
}

void FolderTransport::setDeviceName(const QString &name)
{
    m_deviceName = name;
}

QString FolderTransport::remoteDevice() const
{
    readManifest();
    return m_remoteDevice;
}

qint64 FolderTransport::manifestUpdatedAt() const
{
    readManifest();
    return m_manifestUpdatedAt;
}

} // namespace Sync
