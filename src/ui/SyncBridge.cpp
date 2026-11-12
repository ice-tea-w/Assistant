#include "ui/SyncBridge.h"
#include "core/ConfigManager.h"
#include "core/sync/DeliveryNaming.h"
#include "core/sync/ElevatedRun.h"
#include "core/sync/FolderTransport.h"
#include "core/sync/SyncCommands.h"
#include "core/net/Identity.h"
#include "core/net/NetTransport.h"
#include "core/net/PairingClient.h"
#include "core/net/PeerAddress.h"
#include "core/net/SyncServer.h"
#include "core/Logger.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QLocale>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace {

QString hashFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

} // namespace

SyncBridge::SyncBridge(QObject *parent)
    : QObject(parent)
{
}

SyncBridge::~SyncBridge()
{
    if (m_netThread)
        stopNetServer();
    if (m_timer)
        m_timer->stop();
    if (m_inboxTimer)
        m_inboxTimer->stop();
    if (m_debounce)
        m_debounce->stop();
    if (m_receiveThread) {
        m_receiveThread->wait();
        m_receiveThread->deleteLater();
        m_receiveThread = nullptr;
    }
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
    }
    m_db.close();
}

bool SyncBridge::init(const QString &dbPath)
{
    m_dbPath = dbPath;
    if (!m_db.open(dbPath)) {
        m_statusText = QStringLiteral("无法打开 sync.db");
        emit statusTextChanged();
        return false;
    }

    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &SyncBridge::syncNow);

    m_inboxTimer = new QTimer(this);
    m_inboxTimer->setInterval(3000);
    connect(m_inboxTimer, &QTimer::timeout, this, &SyncBridge::pollInbox);
    m_inboxTimer->start();

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(2000);
    connect(m_debounce, &QTimer::timeout, this, &SyncBridge::syncNow);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { queueSync(); });

    if (m_config) {
        m_roots           = m_config->syncRoots();
        m_sharedDir       = m_config->syncSharedDir();
        m_deviceName      = m_config->syncDeviceName();
        m_intervalMinutes = m_config->syncAutoIntervalMinutes();
        m_watchFiles      = m_config->syncWatchFiles();
        m_inboxDir        = m_config->syncInboxDir();

        // 旧配置只有一个 localRoot，迁移成列表里的一项
        if (m_roots.isEmpty() && !m_config->syncLocalRoot().isEmpty()) {
            const QString p = QDir::cleanPath(m_config->syncLocalRoot());
            QVariantMap one;
            one.insert(QStringLiteral("localPath"), p);
            one.insert(QStringLiteral("alias"), QFileInfo(p).fileName());
            m_roots.append(one);
            m_config->setSyncRoots(m_roots);
        }
    }
    if (m_deviceName.isEmpty())
        m_deviceName = QSysInfo::machineHostName();
    m_autoSync = m_intervalMinutes > 0;

    // 对端名：首次同步后从共享清单里读出来，启动时先空着
    emit pathsChanged();
    emit rootsChanged();
    emit peerNameChanged();
    applyAutoSync();
    refreshChannel();
    refreshConflicts();
    ensureDeviceId();
    refreshPairedDevices();
    startWatchers();
    setStatus(m_roots.isEmpty() || !hasPeerCredentials()
                  ? QStringLiteral("请先添加要同步的文件夹，并与主机配对")
                  : QStringLiteral("尚未同步"));
    return true;
}

void SyncBridge::setConfigManager(ConfigManager *cfg)
{
    m_config = cfg;
}

// ---- getters ----

QString SyncBridge::statusText() const    { return m_statusText; }
bool    SyncBridge::busy() const          { return m_busy; }
int     SyncBridge::progress() const      { return m_progress; }
QVariantList SyncBridge::conflicts() const { return m_conflicts; }
QString SyncBridge::peerName() const      { return m_peerName.isEmpty() ? QStringLiteral("尚未同步") : m_peerName; }
QString SyncBridge::activeChannel() const { return m_activeChannel.isEmpty() ? QStringLiteral("无") : m_activeChannel; }
QVariantList SyncBridge::roots() const     { return m_roots; }
QString SyncBridge::sharedDir() const     { return m_sharedDir; }
QString SyncBridge::deviceName() const    { return m_deviceName; }
bool    SyncBridge::autoSync() const      { return m_autoSync; }
int     SyncBridge::intervalMinutes() const { return m_intervalMinutes; }
bool    SyncBridge::watchFiles() const    { return m_watchFiles; }
QString SyncBridge::inboxDir() const      { return inboxPath(); }
bool    SyncBridge::sending() const       { return m_sending; }
QString SyncBridge::sendStatus() const    { return m_sendStatus; }
QString SyncBridge::shareStatus() const   { return m_shareStatus; }
bool    SyncBridge::shareBusy() const     { return m_shareBusy; }
QString SyncBridge::shareAddress() const  { return m_shareAddress; }

// ---- private setters ----

void SyncBridge::setStatus(const QString &text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    emit statusTextChanged();
}

void SyncBridge::setBusy(bool b)
{
    if (m_busy == b)
        return;
    m_busy = b;
    emit busyChanged();
}

void SyncBridge::setProgress(int p)
{
    p = qBound(0, p, 100);
    if (m_progress == p)
        return;
    m_progress = p;
    emit progressChanged();
}

// ---- 配置 ----

QString SyncBridge::toLocalPath(const QString &urlOrPath) const
{
    const QString s = urlOrPath.trimmed();
    if (s.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
        return QDir::toNativeSeparators(QUrl(s).toLocalFile());
    return s;
}

QString SyncBridge::localIpv4() const
{
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack))
            continue;
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (addr.isLoopback() || addr.isLinkLocal())
                continue;
            return addr.toString();
        }
    }
    return QString();
}

namespace {

QString interfaceKind(QNetworkInterface::InterfaceType type)
{
    switch (type) {
    case QNetworkInterface::Ethernet:  return QStringLiteral("有线");
    case QNetworkInterface::Wifi:      return QStringLiteral("无线");
    case QNetworkInterface::Virtual:   return QStringLiteral("虚拟");
    case QNetworkInterface::Loopback:  return QStringLiteral("回环");
    case QNetworkInterface::Ppp:       return QStringLiteral("拨号");
    default:                           return QStringLiteral("其它");
    }
}

/// 明显是虚拟机/容器网卡：能给对端填，但通常连不到
bool looksLikeVmAdapter(const QString &name)
{
    static const QStringList markers = {
        QStringLiteral("VMware"), QStringLiteral("VirtualBox"), QStringLiteral("vEthernet"),
        QStringLiteral("Hyper-V"), QStringLiteral("Tailscale"),
    };
    for (const QString &m : markers) {
        if (name.contains(m, Qt::CaseInsensitive))
            return m != QStringLiteral("Tailscale");  // Tailscale 虽是虚拟网卡，但可用
    }
    return false;
}

/// 能拿去当直连地址的 IPv6 全局单播（排除链路本地/回环/ULA/Teredo/6to4/映射地址）
bool isUsableGlobalIpv6(const QHostAddress &addr)
{
    if (addr.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    if (addr.isNull() || addr.isLoopback() || addr.isLinkLocal())
        return false;
    const QString text = addr.toString();
    if (text.startsWith(QStringLiteral("fd")) || text.startsWith(QStringLiteral("fc")))
        return false;  // ULA：只在 tailnet / 局域网内有效
    if (text.startsWith(QStringLiteral("2001:0:")))
        return false;  // Teredo
    if (text.startsWith(QStringLiteral("2002:")))
        return false;  // 6to4
    if (text.startsWith(QStringLiteral("::ffff:")))
        return false;  // IPv4 映射
    return true;
}

} // namespace

QVariantList SyncBridge::addressCandidates(quint16 port) const
{
    QVariantList out;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (addr.isLoopback() || addr.isLinkLocal())
                continue;
            const QString address = QStringLiteral("%1:%2").arg(addr.toString()).arg(port);
            bool duplicate = false;
            for (const QVariant &existing : out) {
                if (existing.toMap().value(QStringLiteral("address")).toString() == address) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;
            QVariantMap row;
            row.insert(QStringLiteral("name"), iface.humanReadableName());
            row.insert(QStringLiteral("kind"), interfaceKind(iface.type()));
            row.insert(QStringLiteral("address"), address);
            row.insert(QStringLiteral("hint"),
                       looksLikeVmAdapter(iface.humanReadableName())
                           ? QStringLiteral("对方通常连不到")
                           : QString());
            row.insert(QStringLiteral("virtual"), looksLikeVmAdapter(iface.humanReadableName()));
            out.append(row);
        }
    }
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap ma = a.toMap();
        const QVariantMap mb = b.toMap();
        const bool va = ma.value(QStringLiteral("virtual")).toBool();
        const bool vb = mb.value(QStringLiteral("virtual")).toBool();
        if (va != vb)
            return !va;  // 可用网卡排前面
        return ma.value(QStringLiteral("name")).toString()
               < mb.value(QStringLiteral("name")).toString();
    });
    return out;
}

void SyncBridge::copyToClipboard(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

void SyncBridge::setSharedDir(const QString &path)
{
    m_sharedDir = toLocalPath(path);
    if (m_config)
        m_config->setSyncSharedDir(m_sharedDir);

    m_manifestMtime = 0;
    m_manifestSize  = 0;

    emit pathsChanged();
    startWatchers();
    refreshChannel();
    setStatus(m_sharedDir.isEmpty() ? QStringLiteral("共享目录已清空")
                                    : QStringLiteral("共享目录已保存"));
}

void SyncBridge::addRoot(const QString &localPath)
{
    QString p = localPath.trimmed();
    if (p.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
        p = QUrl(p).toLocalFile();
    const QString path = QDir::cleanPath(p);
    if (path.isEmpty())
        return;
    if (!QDir(path).exists()) {
        setStatus(QStringLiteral("文件夹不存在：") + path);
        return;
    }
    for (const QVariant &v : m_roots) {
        if (QDir::cleanPath(v.toMap().value(QStringLiteral("localPath")).toString()) == path) {
            setStatus(QStringLiteral("这个文件夹已经在同步列表里了"));
            return;
        }
    }

    QSet<QString> taken;
    for (const QVariant &v : m_roots)
        taken.insert(v.toMap().value(QStringLiteral("alias")).toString());

    const QString base = QFileInfo(path).fileName().isEmpty()
                             ? QStringLiteral("文件夹")
                             : QFileInfo(path).fileName();
    QString alias = base;
    for (int i = 2; taken.contains(alias); ++i)
        alias = QStringLiteral("%1-%2").arg(base).arg(i);

    QVariantMap one;
    one.insert(QStringLiteral("localPath"), path);
    one.insert(QStringLiteral("alias"), alias);
    m_roots.append(one);

    if (m_config)
        m_config->setSyncRoots(m_roots);
    emit rootsChanged();
    startWatchers();
    setStatus(QStringLiteral("已添加：") + alias);
}

void SyncBridge::removeRoot(const QString &alias)
{
    QVariantList kept;
    for (const QVariant &v : m_roots) {
        if (v.toMap().value(QStringLiteral("alias")).toString() == alias)
            continue;
        kept.append(v);
    }
    if (kept.size() == m_roots.size())
        return;

    m_roots = kept;
    if (m_config)
        m_config->setSyncRoots(m_roots);
    emit rootsChanged();
    startWatchers();
    setStatus(QStringLiteral("已移除：") + alias);
}

void SyncBridge::setRootAlias(const QString &oldAlias, const QString &newAlias)
{
    const QString fresh = newAlias.trimmed();
    if (fresh.isEmpty() || fresh.contains(QLatin1Char('/')) || fresh.contains(QLatin1Char('\\'))) {
        setStatus(QStringLiteral("别名不能为空，也不能含斜杠"));
        emit rootsChanged();                 // 让界面回退到旧值
        return;
    }

    for (const QVariant &v : m_roots) {
        const QVariantMap m = v.toMap();
        const QString a = m.value(QStringLiteral("alias")).toString();
        if (a == oldAlias)
            continue;
        if (a == fresh) {
            setStatus(QStringLiteral("别名「%1」已被占用").arg(fresh));
            emit rootsChanged();
            return;
        }
    }

    for (int i = 0; i < m_roots.size(); ++i) {
        QVariantMap m = m_roots.at(i).toMap();
        if (m.value(QStringLiteral("alias")).toString() != oldAlias)
            continue;
        m.insert(QStringLiteral("alias"), fresh);
        m_roots[i] = m;
        if (m_config)
            m_config->setSyncRoots(m_roots);
        emit rootsChanged();
        setStatus(QStringLiteral("别名已改为：") + fresh);
        return;
    }
}

QString SyncBridge::absForRel(const QString &relPath) const
{
    const int slash = relPath.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QString();
    const QString alias = relPath.left(slash);
    const QString inner = relPath.mid(slash + 1);
    for (const QVariant &v : m_roots) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("alias")).toString() == alias)
            return QDir(m.value(QStringLiteral("localPath")).toString()).absoluteFilePath(inner);
    }
    return QString();
}

void SyncBridge::addSharedDir(const QString &path)
{
    setSharedDir(path);
}

void SyncBridge::removeSharedDir(const QString &path)
{
    Q_UNUSED(path);
    setSharedDir(QString());
}

void SyncBridge::setAutoSyncInterval(int minutes)
{
    m_intervalMinutes = qMax(0, minutes);
    m_autoSync = m_intervalMinutes > 0;
    if (m_config)
        m_config->setSyncAutoIntervalMinutes(m_intervalMinutes);
    applyAutoSync();
}

void SyncBridge::setAutoSync(bool enabled)
{
    m_autoSync = enabled && m_intervalMinutes > 0;
    applyAutoSync();
}

void SyncBridge::applyAutoSync()
{
    if (!m_timer)
        return;
    if (m_autoSync && m_intervalMinutes > 0)
        m_timer->start(m_intervalMinutes * 60 * 1000);
    else
        m_timer->stop();
    emit autoSyncChanged();
}

// ---- 通道与冲突列表 ----

void SyncBridge::refreshChannel()
{
    // 直连为主：通道只反映「有没有和主机配对」，共享目录不再算一条通道
    QString ch;
    QString peer;
    if (hasPeerCredentials()) {
        ch = QStringLiteral("直连");
        const Sync::Net::PeerAddress parsed =
            Sync::Net::parsePeerAddress(m_config->syncPeerAddress(),
                                       quint16(m_config->syncPort()));
        if (parsed.ok)
            peer = QStringLiteral("%1:%2").arg(parsed.host).arg(parsed.port);
    }
    if (ch != m_activeChannel) {
        m_activeChannel = ch;
        emit activeChannelChanged();
    }
    if (!peer.isEmpty() && peer != m_peerName) {
        m_peerName = peer;
        emit peerNameChanged();
    }
}

void SyncBridge::refreshConflicts()
{
    QVariantList out;
    const QList<ConflictRecord> recs = m_db.recentConflicts(50);
    int index = 0;
    for (const ConflictRecord &r : recs) {
        QVariantMap m;
        m.insert(QStringLiteral("index"),        index++);
        m.insert(QStringLiteral("id"),           r.id);
        m.insert(QStringLiteral("relPath"),      r.relPath);
        m.insert(QStringLiteral("conflictCopy"), r.conflictCopy);
        m.insert(QStringLiteral("localHash"),    r.localHash.left(8));
        m.insert(QStringLiteral("remoteHash"),   r.remoteHash.left(8));
        m.insert(QStringLiteral("detectedAt"),
                 QDateTime::fromSecsSinceEpoch(r.detectedAt).toString(QStringLiteral("MM-dd HH:mm")));
        m.insert(QStringLiteral("resolved"),     r.resolved);
        out.append(m);
    }
    m_conflicts = out;
    emit conflictsChanged();
}

void SyncBridge::openPath(const QString &path)
{
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void SyncBridge::openConflictFolder(int index)
{
    if (index < 0 || index >= m_conflicts.size())
        return;

    const QVariantMap m = m_conflicts.at(index).toMap();
    const QString rel = m.value(QStringLiteral("conflictCopy")).toString();
    const QString firstRoot = m_roots.isEmpty()
                                  ? QString()
                                  : m_roots.first().toMap().value(QStringLiteral("localPath")).toString();
    if (rel.isEmpty()) {
        openPath(firstRoot);
        return;
    }

    const QString abs = absForRel(rel);
    if (abs.isEmpty()) {
        openPath(firstRoot);
        return;
    }
    openPath(QFileInfo(abs).exists() ? QFileInfo(abs).absolutePath() : firstRoot);
}

// ---- 直连（ASYN v2）----

bool SyncBridge::netRunning() const { return m_netRunning; }
QString SyncBridge::netAddress() const { return m_netAddress; }
QVariantList SyncBridge::netAddresses() const { return m_netAddresses; }
QString SyncBridge::netStatus() const { return m_netStatus; }
QString SyncBridge::pairCode() const { return m_pairCode; }
bool SyncBridge::pairActive() const { return m_pairActive; }
QVariantList SyncBridge::pairedDevices() const { return m_pairedDevices; }
bool SyncBridge::peerPaired() const { return hasPeerCredentials(); }

bool SyncBridge::hasPeerCredentials() const
{
    return m_config
        && !m_config->syncPeerAddress().isEmpty()
        && !m_config->syncPeerFingerprint().isEmpty()
        && !m_config->syncPeerSecret().isEmpty();
}

QString SyncBridge::identityDir() const
{
    return QFileInfo(m_dbPath).absolutePath() + QStringLiteral("/identity");
}

QString SyncBridge::ensureDeviceId()
{
    if (m_config) {
        const QString existing = m_config->syncDeviceId();
        if (!existing.isEmpty())
            return existing;
    }
    const QString id = QStringLiteral("dev-%1")
        .arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'));
    if (m_config)
        m_config->setSyncDeviceId(id);
    return id;
}

void SyncBridge::setNetStatus(const QString &text)
{
    if (m_netStatus == text)
        return;
    m_netStatus = text;
    Logger::get().info("SyncBridge/直连: {}", text.toStdString());
    emit netChanged();
}

void SyncBridge::refreshPairedDevices()
{
    QVariantList list;
    const QList<DeviceRecord> records = m_db.allDevices();
    for (const DeviceRecord &rec : records) {
        QVariantMap one;
        one.insert(QStringLiteral("deviceId"), rec.deviceId);
        one.insert(QStringLiteral("fingerprint"), rec.fingerprint);
        one.insert(QStringLiteral("pairedAt"), rec.pairedAt);
        one.insert(QStringLiteral("lastSeenAt"), rec.lastSeenAt);
        list.append(one);
    }
    m_pairedDevices = list;
    emit pairedDevicesChanged();
}

void SyncBridge::startNetServer()
{
    if (m_netRunning || m_netThread) {
        setNetStatus(QStringLiteral("服务端已在运行"));
        return;
    }
    QList<Sync::Net::ServedRoot> served;
    for (const QVariant &v : m_roots) {
        const QVariantMap map = v.toMap();
        Sync::Net::ServedRoot sr;
        sr.localPath = map.value(QStringLiteral("localPath")).toString();
        sr.alias     = map.value(QStringLiteral("alias")).toString();
        if (sr.localPath.isEmpty())
            continue;
        if (sr.alias.isEmpty())
            sr.alias = QFileInfo(sr.localPath).fileName();
        served.append(sr);
    }
    if (served.isEmpty()) {
        setNetStatus(QStringLiteral("先在下面添加至少一个同步文件夹：服务端开放的就是这些文件夹"));
        return;
    }
    for (const Sync::Net::ServedRoot &sr : served) {
        if (!QDir(sr.localPath).exists()) {
            setNetStatus(QStringLiteral("同步文件夹不存在：%1").arg(sr.localPath));
            return;
        }
    }

    const Sync::Net::Identity identity =
        Sync::Net::loadOrCreateIdentity(identityDir(), m_deviceName);
    if (!identity.isValid()) {
        setNetStatus(QStringLiteral("身份不可用：%1").arg(identity.error));
        return;
    }

    // 口令快照进内存：服务端线程不能碰主线程的 QSqlDatabase 连接
    {
        QMutexLocker locker(&m_netMutex);
        m_netSecrets.clear();
        const QList<DeviceRecord> records = m_db.allDevices();
        for (const DeviceRecord &rec : records)
            m_netSecrets.insert(rec.deviceId, rec.secret);
    }

    const QList<Sync::Net::ServedRoot> roots = served;
    const quint16 wantPort = quint16(m_config ? m_config->syncPort() : 45871);
    m_netRunning = true;
    setNetStatus(QStringLiteral("正在监听 %1 端口…").arg(wantPort));

    m_netThread = QThread::create([this, identity, roots, wantPort] {
        auto *server = new Sync::Net::SyncServer(identity, [this](const QString &deviceId) {
            QMutexLocker locker(&m_netMutex);
            return m_netSecrets.value(deviceId);
        });
        server->setPairSink([this](const Sync::Net::SyncServer::PairedDevice &dev) {
            {
                QMutexLocker locker(&m_netMutex);
                m_netSecrets.insert(dev.deviceId, dev.secret);
            }
            // 配对回调发生在服务端线程：落库必须回主线程做
            QMetaObject::invokeMethod(this, [this, dev] {
                DeviceRecord rec;
                rec.deviceId = dev.deviceId;
                rec.fingerprint = dev.fingerprint;
                rec.secret = dev.secret;
                rec.pairedAt = dev.pairedAt;
                rec.lastSeenAt = dev.pairedAt;
                if (!m_db.upsertDevice(rec))
                    setNetStatus(QStringLiteral("配对成功，但写入 devices 表失败"));
                else
                    setNetStatus(QStringLiteral("已配对设备「%1」").arg(dev.name));
                refreshPairedDevices();
                emit pairChanged();
            }, Qt::QueuedConnection);
        });

        if (!server->listen(wantPort, roots)) {
            const QString err = server->lastError();
            delete server;
            QMetaObject::invokeMethod(this, [this, err] {
                m_netRunning = false;
                setNetStatus(QStringLiteral("监听失败：%1").arg(err));
            }, Qt::QueuedConnection);
            return;
        }

        m_netServer = server;
        QEventLoop loop;
        {
            QMutexLocker locker(&m_netMutex);
            m_netLoop = &loop;
        }
        const quint16 actualPort = server->port();
        const QString fingerprint = identity.fingerprint;
        QMetaObject::invokeMethod(this, [this, actualPort, fingerprint] {
            m_netAddresses = addressCandidates(actualPort);
            const QString host = localIpv4().isEmpty() ? m_deviceName : localIpv4();
            m_netAddress = QStringLiteral("%1:%2").arg(host).arg(actualPort);
            setNetStatus(QStringLiteral("正在监听 %1 · 本机指纹 %2")
                             .arg(m_netAddress)
                             .arg(fingerprint.left(16)));
            emit netChanged();
        }, Qt::QueuedConnection);

        loop.exec();

        {
            QMutexLocker locker(&m_netMutex);
            m_netLoop = nullptr;
        }
        m_netServer = nullptr;
        delete server;
    });

    QThread *thread = m_netThread;
    connect(thread, &QThread::finished, this, [this, thread] {
        if (m_netThread == thread) {
            m_netThread = nullptr;
            m_netRunning = false;
            m_netAddress.clear();
            m_netAddresses.clear();
            emit netChanged();
        }
        thread->deleteLater();
    });
    thread->start();
}

void SyncBridge::openFirewallPort()
{
    const int port = m_config ? m_config->syncPort() : 45871;
    setNetStatus(QStringLiteral("正在申请管理员权限，放行 TCP %1…").arg(port));
    auto *worker = QThread::create([this, port] {
        const int code = Sync::Os::runElevated(
            QStringLiteral("netsh"),
            {QStringLiteral("advfirewall"), QStringLiteral("firewall"), QStringLiteral("add"),
             QStringLiteral("rule"), QStringLiteral("name=Assistant Direct %1").arg(port),
             QStringLiteral("dir=in"), QStringLiteral("action=allow"),
             QStringLiteral("protocol=TCP"), QStringLiteral("localport=%1").arg(port),
             QStringLiteral("profile=private,public")});
        QMetaObject::invokeMethod(this, [this, code, port] {
            if (code == 0) {
                setNetStatus(QStringLiteral("已放行 TCP %1（专用 + 公用网络）").arg(port));
            } else if (code == Sync::Os::kElevationCancelled) {
                setNetStatus(QStringLiteral("已取消：没有放行防火墙，对端会被拦"));
            } else {
                setNetStatus(QStringLiteral("放行失败（退出码 %1）").arg(code));
            }
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, this, [worker] { worker->deleteLater(); });
    worker->start();
}

QString SyncBridge::selfCheck()
{
    const int port = m_config ? m_config->syncPort() : 45871;
    QStringList lines;

    lines << (m_netRunning ? QStringLiteral("✓ 监听：正在监听 TCP %1").arg(port)
                           : QStringLiteral("· 监听：未开启（主机要先点「开始监听」）"));

    if (m_netAddresses.isEmpty()) {
        lines << QStringLiteral("· 地址：还没有可用的 IPv4 地址");
    } else {
        QStringList addresses;
        for (const QVariant &value : m_netAddresses) {
            addresses << value.toMap().value(QStringLiteral("address")).toString();
        }
        lines << QStringLiteral("✓ 地址：%1").arg(addresses.join(QStringLiteral("  ")));
    }

    QString firewallError;
    const int firewallCode = Sync::Os::runProgram(
        QStringLiteral("netsh"),
        {QStringLiteral("advfirewall"), QStringLiteral("firewall"), QStringLiteral("show"),
         QStringLiteral("rule"), QStringLiteral("name=Assistant Direct %1").arg(port)},
        &firewallError);
    lines << (firewallCode == 0
                  ? QStringLiteral("✓ 防火墙：已有放行规则")
                  : QStringLiteral("✗ 防火墙：没有放行规则（公用网络下对端会被拦，点「放行防火墙」）"));

    bool hasGlobalIpv6 = false;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (isUsableGlobalIpv6(entry.ip())) {
                hasGlobalIpv6 = true;
                break;
            }
        }
        if (hasGlobalIpv6)
            break;
    }
    lines << (hasGlobalIpv6
                  ? QStringLiteral("· IPv6：有公网单播地址（能否直连还取决于运营商是否放行进站）")
                  : QStringLiteral("· IPv6：只有链路本地（跨公网直连走 Tailscale 或端口映射）"));

    lines << (hasPeerCredentials()
                  ? QStringLiteral("✓ 配对：已记住主机 %1")
                        .arg(m_config ? m_config->syncPeerAddress() : QString())
                  : QStringLiteral("· 配对：未配对（连接方要填主机地址 + 6 位码）"));

    return lines.join(QLatin1Char('\n'));
}

void SyncBridge::stopNetServer()
{
    QObject *context = nullptr;
    {
        QMutexLocker locker(&m_netMutex);
        context = m_netServer;
    }
    if (context && m_netRunning) {
        QMetaObject::invokeMethod(context, [this] {
            if (auto *server = static_cast<Sync::Net::SyncServer *>(m_netServer))
                server->close();
            if (m_netLoop)
                m_netLoop->quit();
        }, Qt::QueuedConnection);
    }
    if (m_netThread && !m_netThread->wait(5000))
        Logger::get().warn("SyncBridge: 直连服务端未在 5 秒内退出");

    m_netRunning = false;
    m_pairActive = false;
    if (!m_pairCode.isEmpty()) {
        m_pairCode.clear();
        emit pairChanged();
    }
    setNetStatus(QStringLiteral("监听已停止"));
}

void SyncBridge::startPairing()
{
    if (!m_netRunning) {
        setNetStatus(QStringLiteral("先点「开始监听」"));
        return;
    }
    auto *server = static_cast<Sync::Net::SyncServer *>(m_netServer);
    if (!server) {
        setNetStatus(QStringLiteral("服务端还没起来"));
        return;
    }

    QString code;
    if (QThread::currentThread() == server->thread()) {
        code = server->openPairingWindow(300);
    } else {
        QMetaObject::invokeMethod(server, [server, &code] {
            code = server->openPairingWindow(300);
        }, Qt::BlockingQueuedConnection);
    }

    m_pairCode = code;
    m_pairActive = !code.isEmpty();
    emit pairChanged();
    setNetStatus(m_pairActive
                     ? QStringLiteral("配对窗口已打开：把 %1 念给对方，5 分钟内有效、只能用一次").arg(code)
                     : QStringLiteral("无法打开配对窗口"));
}

void SyncBridge::stopPairing()
{
    auto *server = static_cast<Sync::Net::SyncServer *>(m_netServer);
    if (server) {
        if (QThread::currentThread() == server->thread()) {
            server->closePairingWindow();
        } else {
            QMetaObject::invokeMethod(server, [server] { server->closePairingWindow(); },
                                      Qt::BlockingQueuedConnection);
        }
    }
    m_pairActive = false;
    m_pairCode.clear();
    emit pairChanged();
    if (m_netRunning)
        setNetStatus(QStringLiteral("配对窗口已关闭"));
}

void SyncBridge::pairWithPeer(const QString &address, const QString &code)
{
    if (!m_config) {
        emit pairingFinished(false, QStringLiteral("配置未就绪"));
        return;
    }
    const QString trimmed = code.trimmed();
    if (trimmed.size() != 6) {
        emit pairingFinished(false, QStringLiteral("配对码是 6 位数字"));
        return;
    }

    const Sync::Net::PeerAddress parsed =
        Sync::Net::parsePeerAddress(address, quint16(m_config->syncPort()));
    if (!parsed.ok) {
        emit pairingFinished(false, parsed.error);
        return;
    }

    const Sync::Net::Identity identity =
        Sync::Net::loadOrCreateIdentity(identityDir(), m_deviceName);
    if (!identity.isValid()) {
        emit pairingFinished(false, QStringLiteral("身份不可用：%1").arg(identity.error));
        return;
    }

    Sync::Net::NetPeer peer;
    peer.host = parsed.host;
    peer.port = parsed.port;
    peer.expectedFingerprint = m_config->syncPeerFingerprint();   // 换主机时先清空
    peer.timeoutMs = 15000;
    const QString clientName = m_deviceName;
    const QString clientFingerprint = identity.fingerprint;

    setNetStatus(QStringLiteral("正在连接 %1…").arg(address));
    auto *worker = QThread::create([this, peer, trimmed, clientName, clientFingerprint] {
        const Sync::Net::PairingOutcome outcome =
            Sync::Net::pairWithHost(peer, trimmed, clientName, clientFingerprint);
        QMetaObject::invokeMethod(this, [this, outcome, peer] {
            if (!outcome.ok) {
                setNetStatus(QStringLiteral("配对失败：%1").arg(outcome.error));
                emit pairingFinished(false, outcome.error);
                return;
            }
            m_config->setSyncPeerAddress(peer.host);
            m_config->setSyncPort(int(peer.port));
            m_config->setSyncPeerFingerprint(outcome.serverFingerprint);
            m_config->setSyncPeerSecret(QString::fromLatin1(outcome.deviceSecret.toBase64()));
            setNetStatus(QStringLiteral("已配对到 %1:%2，同步会走直连").arg(peer.host).arg(peer.port));
            emit pairingFinished(true, QStringLiteral("配对成功"));
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, this, [worker] { worker->deleteLater(); });
    worker->start();
}

void SyncBridge::unpairPeer()
{
    if (!m_config)
        return;
    m_config->setSyncPeerFingerprint(QString());
    m_config->setSyncPeerSecret(QString());
    m_config->setSyncPeerAddress(QString());
    setNetStatus(QStringLiteral("已解除与本机的直连配对"));
    refreshChannel();
    emit pairingFinished(true, QStringLiteral("已解除配对"));
}

void SyncBridge::forgetDevice(const QString &deviceId)
{
    {
        QMutexLocker locker(&m_netMutex);
        m_netSecrets.remove(deviceId);
    }
    if (!m_db.removeDevice(deviceId)) {
        setNetStatus(QStringLiteral("解除授权失败：%1").arg(deviceId));
        return;
    }
    setNetStatus(QStringLiteral("已解除设备 %1 的授权").arg(deviceId));
    refreshPairedDevices();
}

// ---- 同步 ----

void SyncBridge::syncNow()
{
    if (m_busy || m_worker) {
        m_pendingSync = true;
        return;
    }

    if (m_roots.isEmpty() || !hasPeerCredentials()) {
        setStatus(QStringLiteral("请先添加要同步的文件夹，并完成与主机的配对"));
        // 同步没跑起来必须有痕迹：界面上只改一行状态，用户回头查不出为什么
        Logger::get().warn("SyncBridge: 跳过同步（同步根 {} 个，已配对 {}）",
                           m_roots.size(), hasPeerCredentials());
        return;
    }

    setBusy(true);
    setProgress(0);
    setStatus(QStringLiteral("正在同步…"));

    // 参数按值捕进工作线程：主线程后续改配置不影响本轮
    QList<Sync::SyncRoot> roots;
    for (const QVariant &v : m_roots) {
        const QVariantMap m = v.toMap();
        roots.append(Sync::SyncRoot{ m.value(QStringLiteral("localPath")).toString(),
                                     m.value(QStringLiteral("alias")).toString() });
    }
    const QString name   = m_deviceName;
    const QString dbPath = m_dbPath;

    // 已配对就再挂一条直连通道；参数在这里定死，避免工作线程里碰 m_config
    Sync::Net::NetPeer netPeer;
    if (hasPeerCredentials()) {
        const Sync::Net::PeerAddress parsed =
            Sync::Net::parsePeerAddress(m_config->syncPeerAddress(),
                                       quint16(m_config->syncPort()));
        if (parsed.ok) {
            netPeer.host = parsed.host;
            netPeer.port = parsed.port;
            netPeer.expectedFingerprint = m_config->syncPeerFingerprint();
            netPeer.deviceId = ensureDeviceId();
            netPeer.deviceSecret = QByteArray::fromBase64(m_config->syncPeerSecret().toLatin1());
        } else {
            Logger::get().warn("SyncBridge: 直连地址无效（{}）", parsed.error.toStdString());
        }
    }

    m_worker = QThread::create([this, roots, name, dbPath, netPeer] {
        // 工作线程自带独立连接名 —— QSqlDatabase 连接不可跨线程
        Sync::SyncResult r;
        SyncDatabase workerDb(QStringLiteral("sync_worker"));
        if (!workerDb.open(dbPath)) {
            r.status  = Sync::SyncResult::Aborted;
            r.message = QStringLiteral("无法打开 sync.db");
        } else {
            Sync::SyncEngine engine;
            engine.setDatabase(&workerDb);
            engine.setRoots(roots);
            engine.setDeviceName(name);

            QList<std::shared_ptr<Sync::ITransport>> transports;
            if (!netPeer.deviceSecret.isEmpty() && !netPeer.host.isEmpty()) {
                transports.append(std::make_shared<Sync::Net::NetTransport>(netPeer));
                Logger::get().info("SyncBridge: 已挂载直连通道 {}:{}", netPeer.host.toStdString(), netPeer.port);
            }
            engine.setTransports(transports);

            connect(&engine, &Sync::SyncEngine::progress, this, &SyncBridge::onEngineProgress);
            connect(&engine, &Sync::SyncEngine::fileHandled, this, &SyncBridge::onEngineFileHandled);

            r = engine.sync();
            workerDb.close();
        }

        QMetaObject::invokeMethod(this, [this, r] { onSyncFinished(r); }, Qt::QueuedConnection);
    });

    QThread *thread = m_worker;
    connect(thread, &QThread::finished, this, [this, thread] {
        if (m_worker == thread) {
            m_worker->deleteLater();
            m_worker = nullptr;
        }
        if (m_pendingSync) {
            m_pendingSync = false;
            if (m_debounce)
                m_debounce->start(3000);
        }
    });
    m_worker->start();
}

void SyncBridge::onEngineProgress(int percent, const QString &file)
{
    setProgress(percent);
    if (!file.isEmpty())
        setStatus(QStringLiteral("正在处理：") + file);
}

void SyncBridge::onEngineFileHandled(const QString &relPath, const QString &action)
{
    emit fileHandled(relPath, action);
}

void SyncBridge::onSyncFinished(const Sync::SyncResult &r)
{
    setBusy(false);
    setProgress(100);
    refreshChannel();
    refreshConflicts();

    QString s;
    switch (r.status) {
    case Sync::SyncResult::Ok: {
        s = QStringLiteral("同步完成：上传 %1 · 下载 %2 · 冲突 %3 · 删除 %4")
                .arg(r.uploaded).arg(r.downloaded).arg(r.conflicts).arg(r.deleted);
        if (r.failed > 0)
            s += QStringLiteral(" · 失败 %1").arg(r.failed);
        if (r.bytesMoved > 0)
            s += QStringLiteral(" · %1").arg(QLocale().formattedDataSize(r.bytesMoved));
        break;
    }
    case Sync::SyncResult::Busy:
        s = r.message.isEmpty() ? QStringLiteral("另一台设备正在同步") : r.message;
        break;
    case Sync::SyncResult::NoChannelAvailable:
    case Sync::SyncResult::RemoteUnreachable:
    case Sync::SyncResult::Aborted:
        s = r.message.isEmpty() ? QStringLiteral("同步未完成") : r.message;
        break;
    }
    setStatus(s);
    if (r.status == Sync::SyncResult::Ok)
        Logger::get().info("SyncBridge: {}", s.toStdString());
    else
        Logger::get().warn("SyncBridge: {}（status={}）", s.toStdString(), int(r.status));
    emit syncFinished(r.uploaded, r.downloaded, r.conflicts);
}

// ---- 投递区 / 收件箱 / 自动触发 ----

void SyncBridge::setSendStatus(const QString &text)
{
    if (m_sendStatus == text)
        return;
    m_sendStatus = text;
    emit sendStatusChanged();
}

void SyncBridge::setShareStatus(const QString &text)
{
    if (m_shareStatus == text)
        return;
    m_shareStatus = text;
    emit shareStatusChanged();
}

QString SyncBridge::inboxPath() const
{
    const QString custom = m_inboxDir.trimmed();
    if (!custom.isEmpty())
        return QDir::fromNativeSeparators(custom);

    QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (docs.isEmpty())
        docs = QDir::homePath();
    return docs + QStringLiteral("/Assistant 收件箱");
}

QString SyncBridge::deliveryDir() const
{
    if (m_sharedDir.isEmpty())
        return QString();
    return QDir::cleanPath(m_sharedDir + QStringLiteral("/_投递"));
}

void SyncBridge::sendFiles(const QStringList &paths)
{
    if (m_sharedDir.isEmpty()) {
        setSendStatus(QStringLiteral("请先设置共享目录"));
        return;
    }
    if (m_sending)
        return;

    QStringList local;
    for (const QString &p : paths) {
        QString path = p;
        if (path.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
            path = QUrl(path).toLocalFile();
        if (QFileInfo(path).isFile())
            local.append(path);
    }
    if (local.isEmpty()) {
        setSendStatus(QStringLiteral("没有可发送的文件"));
        return;
    }

    m_sending = true;
    emit sendingChanged();
    setSendStatus(QStringLiteral("正在发送 %1 个文件…").arg(local.size()));

    const QString shared = m_sharedDir;
    const QString device = m_deviceName;

    auto worker = QThread::create([this, local, shared, device] {
        const QString dir = QDir::cleanPath(shared + QStringLiteral("/_投递/")
                                            + Sync::sanitizeDeviceName(device));
        QDir().mkpath(dir);

        int ok = 0;
        int fail = 0;
        for (const QString &src : local) {
            const QString original = QFileInfo(src).fileName();
            const QString base = Sync::makeDeliveryFileName(original, QDateTime::currentDateTime());

            QString dst = dir + QLatin1Char('/') + base;
            for (int seq = 2; QFileInfo::exists(dst); ++seq)
                dst = dir + QLatin1Char('/') + base + QStringLiteral("_%1").arg(seq);

            const QString tmp = dst + QStringLiteral(".part");
            QFile::remove(tmp);
            if (QFile::copy(src, tmp) && QFile::rename(tmp, dst))
                ++ok;
            else {
                QFile::remove(tmp);
                ++fail;
            }
        }

        QMetaObject::invokeMethod(this, [this, ok, fail] {
            m_sending = false;
            emit sendingChanged();
            if (ok > 0) {
                setSendStatus(QStringLiteral("已发送 %1 个文件").arg(ok));
                Logger::get().info("SyncBridge: 投递 {} 个文件到共享目录", ok);
            } else {
                setSendStatus(QStringLiteral("发送失败（%1 个文件）").arg(fail));
                Logger::get().warn("SyncBridge: 投递失败 {} 个文件", fail);
            }
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, this, [worker] { worker->deleteLater(); });
    worker->start();
}

// ---- 一键共享 / 一键连接 ----

QString SyncBridge::shareNameSuggestion(const QString &localPath) const
{
    const QString path = toLocalPath(localPath.trimmed());
    return Sync::suggestShareName(path.isEmpty() ? m_sharedDir : path);
}

void SyncBridge::shareFolder(const QString &localPath, const QString &shareName,
                             const QString &user)
{
    if (m_shareBusy)
        return;

    const QString path = QDir::cleanPath(toLocalPath(localPath.trimmed()));
    if (path.isEmpty() || !QDir(path).exists()) {
        const QString msg = QStringLiteral("目录不存在：%1").arg(path);
        setShareStatus(msg);
        emit shareOpFinished(false, msg);
        return;
    }

    const QString name = shareName.trimmed().isEmpty() ? Sync::suggestShareName(path)
                                                      : shareName.trimmed();
    if (!Sync::isValidShareName(name)) {
        const QString msg = QStringLiteral(
            "共享名不合法：不能用 \\ / : * ? \" < > |，1–80 个字符，首尾不能是空格");
        setShareStatus(msg);
        emit shareOpFinished(false, msg);
        return;
    }

    // 默认只授权当前登录账号；要给对方用就在界面里填对方的账号名
    const QString account =
        user.trimmed().isEmpty() ? qEnvironmentVariable("USERNAME") : user.trimmed();

    const QString batPath = QDir::tempPath()
                            + QStringLiteral("/assistant-share-%1.bat")
                                  .arg(QCoreApplication::applicationPid());
    QFile bat(batPath);
    if (!bat.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString msg = QStringLiteral("写不了临时脚本 %1：%2").arg(batPath, bat.errorString());
        setShareStatus(msg);
        emit shareOpFinished(false, msg);
        return;
    }
    bat.write(Sync::shareScript(path, name, account).toUtf8());
    bat.close();

    m_shareBusy = true;
    emit shareBusyChanged();
    setShareStatus(QStringLiteral("正在申请管理员权限共享「%1」…")
                       .arg(QDir::toNativeSeparators(path)));

    auto worker = QThread::create([this, batPath, path, name] {
        const int code = Sync::Os::runElevated(QStringLiteral("cmd.exe"),
                                              {QStringLiteral("/c"), batPath});
        QFile::remove(batPath);

        const QString unc = QStringLiteral("\\\\%1\\%2").arg(QSysInfo::machineHostName(), name);
        QString msg;
        if (code == 0)
            msg = QStringLiteral("已共享为 %1（共享权限与 NTFS 都给到「修改」）").arg(unc);
        else if (code == Sync::Os::kElevationCancelled)
            msg = QStringLiteral("已取消：创建共享需要管理员权限");
        else
            msg = Sync::explainExitCode(code, QDir::toNativeSeparators(path));

        QMetaObject::invokeMethod(this, [this, code, msg, unc, path] {
            m_shareBusy = false;
            emit shareBusyChanged();
            if (code == 0) {
                m_shareAddress = unc;
                emit shareAddressChanged();
                // 主机通常就是这个目录做共享根；已经有共享目录时不覆盖用户的选择
                if (m_sharedDir.isEmpty())
                    setSharedDir(path);
                Logger::get().info("SyncBridge: 已共享 {} → {}", path.toStdString(),
                                   unc.toStdString());
            }
            setShareStatus(msg);
            emit shareOpFinished(code == 0, msg);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, this, [worker] { worker->deleteLater(); });
    worker->start();
}

void SyncBridge::connectShared(const QString &uncPath, const QString &user,
                               const QString &password)
{
    if (m_shareBusy)
        return;

    QString path = QDir::fromNativeSeparators(uncPath.trimmed());
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (!path.startsWith(QStringLiteral("//"))) {
        const QString msg =
            QStringLiteral("请填对方给你的网络路径，形如 \\\\主机名\\共享名");
        setShareStatus(msg);
        emit shareOpFinished(false, msg);
        return;
    }

    const QString native = QDir::toNativeSeparators(path);
    const QStringList args = Sync::connectArgs(native, user.trimmed(), password);

    m_shareBusy = true;
    emit shareBusyChanged();
    setShareStatus(QStringLiteral("正在连接 %1…").arg(native));

    auto worker = QThread::create([this, args, native] {
        QString err;
        const int code = Sync::Os::runProgram(QStringLiteral("net"), args, &err);

        QString msg;
        if (code == 0) {
            msg = QStringLiteral("已连接 %1").arg(native);
        } else {
            msg = Sync::explainExitCode(code, native);
            if (!err.isEmpty())
                msg += QStringLiteral("（%1）").arg(err);
        }

        QMetaObject::invokeMethod(this, [this, code, msg, native] {
            m_shareBusy = false;
            emit shareBusyChanged();
            if (code == 0) {
                setSharedDir(native);
                Logger::get().info("SyncBridge: 已连接共享 {}", native.toStdString());
            }
            setShareStatus(msg);
            emit shareOpFinished(code == 0, msg);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, this, [worker] { worker->deleteLater(); });
    worker->start();
}

QVariantMap SyncBridge::checkShared()
{
    QVariantMap out;
    const QString dir = m_sharedDir.trimmed();
    if (dir.isEmpty()) {
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("message"), QStringLiteral("还没有填共享目录"));
        return out;
    }

    Sync::FolderTransport t(dir);
    t.setDeviceName(m_deviceName);
    if (!t.probe()) {
        out.insert(QStringLiteral("ok"), false);
        const QString why = t.probeError();
        out.insert(QStringLiteral("message"),
                   why.isEmpty() ? QStringLiteral("共享目录不可访问或不可写：%1").arg(dir) : why);
        return out;
    }

    const bool    hasManifest = t.hasManifest();
    const int     count       = t.listRemote().size();
    const QString device      = t.remoteDevice();
    const qint64  updated     = t.manifestUpdatedAt();

    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("hasManifest"), hasManifest);
    out.insert(QStringLiteral("device"), device);
    out.insert(QStringLiteral("fileCount"), count);
    QVariantList aliasList;
    for (const QString &a : t.remoteAliases())
        aliasList.append(a);
    out.insert(QStringLiteral("aliases"), aliasList);
    out.insert(QStringLiteral("updatedAt"),
               updated > 0 ? QDateTime::fromSecsSinceEpoch(updated)
                                 .toString(QStringLiteral("MM-dd HH:mm"))
                           : QString());

    QString msg;
    if (!hasManifest)
        msg = QStringLiteral("目录可写，但对端还没有同步过（没有清单）");
    else if (device.isEmpty())
        msg = QStringLiteral("清单里的设备名是空的（可能是老版本写的），文件数 %1").arg(count);
    else
        msg = QStringLiteral("与「%1」共享正常，清单里有 %2 个文件").arg(device).arg(count);
    out.insert(QStringLiteral("message"), msg);
    return out;
}

void SyncBridge::openInbox()
{
    const QString dir = inboxPath();
    QDir().mkpath(dir);
    openPath(dir);
}

void SyncBridge::setWatchFiles(bool enabled)
{
    if (m_watchFiles == enabled)
        return;
    m_watchFiles = enabled;
    if (m_config)
        m_config->setSyncWatchFiles(enabled);
    startWatchers();
    emit watchFilesChanged();
    setStatus(enabled ? QStringLiteral("已开启：文件变化时自动同步")
                      : QStringLiteral("已关闭：文件变化时自动同步"));
}

void SyncBridge::setDeviceName(const QString &name)
{
    const QString fresh = name.trimmed();
    if (fresh.isEmpty()) {
        setStatus(QStringLiteral("设备名不能为空"));
        return;
    }
    if (fresh == m_deviceName)
        return;

    m_deviceName = fresh;
    if (m_config)
        m_config->setSyncDeviceName(fresh);
    emit pathsChanged();
    setStatus(QStringLiteral("设备名已保存：") + fresh);
}

void SyncBridge::queueSync()
{
    if (m_busy || m_worker) {
        m_pendingSync = true;
        return;
    }
    if (m_debounce && !m_debounce->isActive())
        m_debounce->start();
}

void SyncBridge::startWatchers()
{
    if (!m_watcher)
        return;

    QStringList old;
    old << m_watcher->directories() << m_watcher->files();
    if (!old.isEmpty())
        m_watcher->removePaths(old);

    if (!m_watchFiles)
        return;

    int budget = 1000;
    bool truncated = false;
    for (const QVariant &v : m_roots) {
        const QString root = v.toMap().value(QStringLiteral("localPath")).toString();
        if (root.isEmpty() || !QDir(root).exists())
            continue;

        QStringList stack{root};
        while (!stack.isEmpty()) {
            if (budget <= 0) {
                truncated = true;
                break;
            }
            const QString dir = stack.takeLast();
            m_watcher->addPath(dir);
            --budget;

            const QFileInfoList subdirs =
                QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QFileInfo &sub : subdirs) {
                if (sub.isSymLink() || sub.isJunction()
                    || sub.fileName() == QLatin1String(".sync"))
                    continue;
                stack.append(sub.filePath());
            }
        }
        if (budget <= 0)
            break;
    }

    const QString meta = QDir::cleanPath(m_sharedDir + QStringLiteral("/.sync"));
    if (!m_sharedDir.isEmpty() && QDir(meta).exists())
        m_watcher->addPath(meta);

    if (truncated)
        Logger::get().warn("SyncBridge: 监视目录超过 1000 个，超出的子目录只靠定时兜底");
}

void SyncBridge::pollInbox()
{
    if (m_sharedDir.isEmpty())
        return;

    // 1) 对端清单变化 → 去抖同步（watcher 在 UNC 上不一定可靠，这里兜底）
    const QString manifest = QDir::cleanPath(m_sharedDir + QStringLiteral("/.sync/manifest.json"));
    const QFileInfo mf(manifest);
    if (mf.exists()) {
        const qint64 mtime = mf.lastModified().toMSecsSinceEpoch();
        const qint64 size  = mf.size();
        if (m_manifestMtime > 0 && (mtime != m_manifestMtime || size != m_manifestSize))
            queueSync();
        m_manifestMtime = mtime;
        m_manifestSize  = size;
    }

    // 2) 领取 _投递/ 里的新文件：按设备分子目录（<_投递>/<设备>/<时间戳>_<原名>），
    //    根下平铺的旧格式文件也顺手兼容。
    if (!m_db.isOpen() || m_receiveThread || m_sending)
        return;

    const QString root = deliveryDir();
    if (root.isEmpty() || !QDir(root).exists())
        return;

    const QSet<QString> known = m_db.receivedPaths();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString selfDir = Sync::sanitizeDeviceName(m_deviceName);

    struct Incoming { QString key; QString path; };
    QList<Incoming> fresh;
    const auto consider = [&](const QString &key, const QFileInfo &fi) {
        if (fresh.size() >= 20)
            return;
        if (fi.fileName().endsWith(QStringLiteral(".part"), Qt::CaseInsensitive))
            return;
        if (known.contains(key))
            return;
        // 太新的文件可能对方还在写，下一轮再来
        if (fi.lastModified().toSecsSinceEpoch() > now - 2)
            return;
        fresh.append({key, fi.absoluteFilePath()});
    };

    const QFileInfoList senders = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                          QDir::Name);
    for (const QFileInfo &sender : senders) {
        if (sender.fileName() == selfDir)
            continue;   // 自己投的目录，不领回本地
        const QFileInfoList files = QDir(sender.filePath()).entryInfoList(QDir::Files, QDir::Name);
        for (const QFileInfo &fi : files)
            consider(sender.fileName() + QLatin1Char('/') + fi.fileName(), fi);
    }

    // 旧格式：文件直接平铺在 _投递/ 根下，靠文件名前缀区分发送者
    const QFileInfoList legacy = QDir(root).entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo &fi : legacy) {
        if (Sync::isOwnDelivery(fi.fileName(), m_deviceName))
            continue;
        consider(fi.fileName(), fi);
    }
    if (fresh.isEmpty())
        return;

    const QString inbox  = inboxPath();
    const QString dbPath = m_dbPath;

    QThread *thread = QThread::create([this, fresh, inbox, dbPath] {
        SyncDatabase db(QStringLiteral("sync_recv"));
        int got = 0;
        if (db.open(dbPath)) {
            QDir().mkpath(inbox);
            for (const Incoming &it : fresh) {
                const QString landed = Sync::placeIncoming(it.path, inbox);
                if (landed.isEmpty())
                    continue;
                db.markReceived(it.key, hashFile(landed));
                ++got;
            }
            db.close();
        }

        QMetaObject::invokeMethod(this, [this, got, inbox] {
            if (got <= 0)
                return;
            setStatus(QStringLiteral("收到 %1 个文件").arg(got));
            Logger::get().info("SyncBridge: 领取 {} 个投递文件 → {}", got, inbox.toStdString());
            emit filesReceived(got, inbox);
        }, Qt::QueuedConnection);
    });

    m_receiveThread = thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        if (m_receiveThread == thread)
            m_receiveThread = nullptr;
        thread->deleteLater();
    });
    thread->start();
}
