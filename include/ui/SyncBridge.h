#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QHash>
#include <QMutex>

#include "database/SyncDatabase.h"
#include "core/sync/SyncEngine.h"

class QThread;
class QTimer;
class QEventLoop;
class QFileSystemWatcher;
class ConfigManager;

namespace Sync::Net {
struct NetPeer;
}

/// 主面板「设备同步」页的 QML↔C++ 桥梁。
///
/// 与 PanelBridge 并列而不是塞进它：PanelBridge 已承载待办/笔记/剪贴板三块，
/// 再加同步会变成新的上帝对象（见设计文档 §4.8）。
class SyncBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString      statusText    READ statusText    NOTIFY statusTextChanged)
    Q_PROPERTY(bool         busy          READ busy          NOTIFY busyChanged)
    Q_PROPERTY(int          progress      READ progress      NOTIFY progressChanged)
    Q_PROPERTY(QVariantList conflicts     READ conflicts     NOTIFY conflictsChanged)
    Q_PROPERTY(QString      peerName      READ peerName      NOTIFY peerNameChanged)
    Q_PROPERTY(QString      activeChannel READ activeChannel NOTIFY activeChannelChanged)
    Q_PROPERTY(QVariantList roots         READ roots         NOTIFY rootsChanged)
    Q_PROPERTY(QString      sharedDir     READ sharedDir     NOTIFY pathsChanged)
    Q_PROPERTY(QString      deviceName    READ deviceName    NOTIFY pathsChanged)
    Q_PROPERTY(bool         autoSync      READ autoSync      NOTIFY autoSyncChanged)
    Q_PROPERTY(int          intervalMinutes READ intervalMinutes NOTIFY autoSyncChanged)
    Q_PROPERTY(bool         watchFiles    READ watchFiles    NOTIFY watchFilesChanged)
    Q_PROPERTY(QString      inboxDir      READ inboxDir      NOTIFY inboxDirChanged)
    Q_PROPERTY(bool         sending       READ sending       NOTIFY sendingChanged)
    Q_PROPERTY(QString      sendStatus    READ sendStatus    NOTIFY sendStatusChanged)
    Q_PROPERTY(QString      shareStatus   READ shareStatus   NOTIFY shareStatusChanged)
    Q_PROPERTY(bool         shareBusy     READ shareBusy     NOTIFY shareBusyChanged)
    Q_PROPERTY(QString      shareAddress  READ shareAddress  NOTIFY shareAddressChanged)
    // ---- 直连（ASYN v2）----
    Q_PROPERTY(bool         netRunning    READ netRunning    NOTIFY netChanged)
    Q_PROPERTY(QString      netAddress    READ netAddress    NOTIFY netChanged)
    Q_PROPERTY(QVariantList netAddresses  READ netAddresses  NOTIFY netChanged)
    Q_PROPERTY(QString      netStatus     READ netStatus     NOTIFY netChanged)
    Q_PROPERTY(QString      pairCode      READ pairCode      NOTIFY pairChanged)
    Q_PROPERTY(bool         pairActive    READ pairActive    NOTIFY pairChanged)
    Q_PROPERTY(QVariantList pairedDevices READ pairedDevices NOTIFY pairedDevicesChanged)
    Q_PROPERTY(bool         peerPaired    READ peerPaired    NOTIFY netChanged)
    // ---- 投递（单文件直发 + 待发队列）----
    Q_PROPERTY(int          outboxCount   READ outboxCount   NOTIFY outboxChanged)
    Q_PROPERTY(QVariantList outboxList    READ outboxList    NOTIFY outboxChanged)

public:
    explicit SyncBridge(QObject *parent = nullptr);
    ~SyncBridge() override;

    /// 打开 sync.db（连接名 "sync"，主线程用）
    bool init(const QString &dbPath);
    void setConfigManager(ConfigManager *cfg);

    QString      statusText() const;
    bool         busy() const;
    int          progress() const;
    QVariantList conflicts() const;
    QString      peerName() const;
    QString      activeChannel() const;
    QVariantList roots() const;
    QString      sharedDir() const;
    QString      deviceName() const;
    bool         autoSync() const;
    int          intervalMinutes() const;
    bool         watchFiles() const;
    QString      inboxDir() const;
    bool         sending() const;
    QString      sendStatus() const;
    QString      shareStatus() const;
    bool         shareBusy() const;
    QString      shareAddress() const;

    bool         netRunning() const;
    QString      netAddress() const;
    QVariantList netAddresses() const;
    QString      netStatus() const;
    QString      pairCode() const;
    bool         pairActive() const;
    QVariantList pairedDevices() const;
    bool         peerPaired() const;
    int          outboxCount() const;
    QVariantList outboxList() const;

    Q_INVOKABLE void addSharedDir(const QString &path);
    Q_INVOKABLE void removeSharedDir(const QString &path);
    Q_INVOKABLE void syncNow();
    Q_INVOKABLE void openConflictFolder(int index);
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void refreshConflicts();
    Q_INVOKABLE void setAutoSyncInterval(int minutes);
    Q_INVOKABLE void setAutoSync(bool enabled);
    /// 往同步列表里加一个本地文件夹，别名自动取目录名（重名自动加 -2/-3）
    Q_INVOKABLE void addRoot(const QString &localPath);
    /// 从同步列表里移除一个文件夹，按别名匹配
    Q_INVOKABLE void removeRoot(const QString &alias);
    /// 改别名。两台电脑上"同一个文件夹"必须用同一个别名才能对上
    Q_INVOKABLE void setRootAlias(const QString &oldAlias, const QString &newAlias);
    /// 写入共享目录并落盘。可传本地路径、UNC 或 file:// URL
    Q_INVOKABLE void setSharedDir(const QString &path);
    /// 把 file:// URL 转成本地/UNC 路径（带 file: 前缀才转，普通路径原样返回）
    Q_INVOKABLE QString toLocalPath(const QString &urlOrPath) const;
    /// 本机非回环 IPv4 地址（没有可用网络时返回空串）
    Q_INVOKABLE QString localIpv4() const;
    /// 复制文本到系统剪贴板
    Q_INVOKABLE void copyToClipboard(const QString &text);
    /// 把文件投递到共享目录的 _投递/ 下。路径可为本地路径或 file:// URL
    Q_INVOKABLE void sendFiles(const QStringList &paths);
    /// 检查共享目录：ok / hasManifest / device / updatedAt / fileCount / message
    Q_INVOKABLE QVariantMap checkShared();
    /// 创建并打开收件箱
    Q_INVOKABLE void openInbox();
    /// 开关「文件变化时自动同步」
    Q_INVOKABLE void setWatchFiles(bool enabled);
    /// 设置本机设备名（写进清单，也是投递文件名前缀）
    Q_INVOKABLE void setDeviceName(const QString &name);

    /// 把本机目录开成共享：UAC 提权跑 net share + icacls，
    /// 成功后 shareAddress 给出 \\主机名\共享名（复制给对方用）
    Q_INVOKABLE void shareFolder(const QString &localPath, const QString &shareName,
                                 const QString &user);
    /// 连到对方的共享：net use，成功后把它设成本机共享目录
    Q_INVOKABLE void connectShared(const QString &uncPath, const QString &user,
                                   const QString &password);
    /// 按目录名推断共享名（前端预填）
    Q_INVOKABLE QString shareNameSuggestion(const QString &localPath) const;

    /// 在本机同步端口上启动 ASYN 服务端，对外提供当前共享目录
    Q_INVOKABLE void startNetServer();
    /// 放行直连端口的入站防火墙规则（会弹 UAC）
    Q_INVOKABLE void openFirewallPort();
    /// 直连前置条件自检：监听/地址/防火墙/IPv6/配对状态，返回多行文本
    Q_INVOKABLE QString selfCheck();
    Q_INVOKABLE void stopNetServer();
    /// 打开配对窗口（5 分钟、一次性），码通过 pairCode 属性给出
    Q_INVOKABLE void startPairing();
    Q_INVOKABLE void stopPairing();
    /// 连接方：用主机地址（host:port）与 6 位码配对，成功后落盘凭据
    Q_INVOKABLE void pairWithPeer(const QString &address, const QString &code);
    /// 连接方：忘掉已配对的主机
    Q_INVOKABLE void unpairPeer();
    /// 主机：解除某台已配对设备的授权
    Q_INVOKABLE void forgetDevice(const QString &deviceId);

    /// 把文件放进待发队列（队友不在线也不丢），随后自动尝试发送
    Q_INVOKABLE void queueSend(const QStringList &paths);
    /// 立刻把队列里到点的条目发出去
    Q_INVOKABLE void sendQueuedNow();
    /// 从队列里删掉一条（放弃发送）
    Q_INVOKABLE void removeQueuedItem(qint64 id);
    /// 从主机的 `_inbox/` 领取投递到本机收件箱
    Q_INVOKABLE void fetchIncoming();
    /// 重新读一遍队列（供 QML 手动刷新）
    Q_INVOKABLE void refreshOutbox();

signals:
    void statusTextChanged();
    void busyChanged();
    void progressChanged();
    void conflictsChanged();
    void peerNameChanged();
    void activeChannelChanged();
    void pathsChanged();
    void rootsChanged();
    void autoSyncChanged();
    void watchFilesChanged();
    void inboxDirChanged();
    void sendingChanged();
    void sendStatusChanged();
    void shareStatusChanged();
    void shareBusyChanged();
    void shareAddressChanged();
    void netChanged();
    void pairChanged();
    void pairedDevicesChanged();
    /// 连接方配对结果（QML 用来弹提示）
    void pairingFinished(bool ok, const QString &message);
    void fileHandled(const QString &relPath, const QString &action);
    void syncFinished(int uploaded, int downloaded, int conflicts);
    /// 从投递区领取了文件（count 个，落在 inboxDir）
    void filesReceived(int count, const QString &inboxDir);
    /// 一键共享/一键连接的结果（QML 用来弹提示）
    void shareOpFinished(bool ok, const QString &message);
    void outboxChanged();
    /// 一轮发送的结果：成功几个、失败几个
    void deliveryFinished(int sent, int failed);

private:
    void onEngineProgress(int percent, const QString &file);
    void onEngineFileHandled(const QString &relPath, const QString &action);
    void onSyncFinished(const Sync::SyncResult &r);
    void refreshChannel();
    void applyAutoSync();
    /// 3 秒轮询：对端清单变化触发同步；_投递/ 下的新文件领取到收件箱
    void pollInbox();
    /// 去抖后请求一轮同步；正在同步时只记 pending
    void queueSync();
    /// 按当前同步根重建 watcher（递归注册目录，上限 1000 个）
    void startWatchers();
    QString inboxPath() const;
    QString deliveryDir() const;
    void setSendStatus(const QString &text);
    void setSending(bool b);
    void setShareStatus(const QString &text);
    /// "<别名>/相对路径" → 本地绝对路径；别名不认识时返回空串
    QString absForRel(const QString &relPath) const;
    void setStatus(const QString &text);
    void setBusy(bool b);
    void setProgress(int p);
    void setNetStatus(const QString &text);
    /// 所有可用 IPv4 监听地址（含网卡名），每项 {name, address}
    QVariantList addressCandidates(quint16 port) const;
    void refreshPairedDevices();
    /// 身份目录：sync.db 同级的 identity/
    QString identityDir() const;
    /// 本机稳定标识，首次调用时生成并写进配置
    QString ensureDeviceId();
    bool    hasPeerCredentials() const;
    /// 按配置拼出直连对端；凭据不全时返回 false
    bool    peerForDelivery(Sync::Net::NetPeer *peer) const;
    /// 投递落点：`_inbox/<设备名>-<deviceId 前 8 位>/<时间戳>_<原名>`
    QString deliveryRelPath(const QString &localPath) const;

    QString       m_dbPath;
    ConfigManager *m_config = nullptr;

    SyncDatabase  m_db{QStringLiteral("sync")};
    QThread      *m_worker = nullptr;
    QTimer       *m_timer  = nullptr;

    QString m_statusText = QStringLiteral("尚未同步");
    bool    m_busy = false;
    int     m_progress = 0;
    QVariantList m_conflicts;
    QString m_peerName;
    QString m_activeChannel;
    QVariantList m_roots;      // 每项 {localPath, alias}
    QString m_sharedDir;
    QString m_deviceName;
    QString m_lastConflictDir;
    bool    m_autoSync = false;
    int     m_intervalMinutes = 0;

    bool         m_watchFiles = true;
    QString      m_inboxDir;
    bool         m_sending = false;
    QString      m_sendStatus;
    QString      m_shareStatus;
    bool         m_shareBusy = false;
    QString      m_shareAddress;

    // ---- 直连 ----
    QThread     *m_netThread = nullptr;
    QObject     *m_netServer = nullptr;   // Sync::Net::SyncServer*，仅当跨线程 context 用
    QEventLoop  *m_netLoop = nullptr;
    QMutex       m_netMutex;              // 保护 m_netSecrets / m_netLoop
    QHash<QString, QByteArray> m_netSecrets;
    bool         m_netRunning = false;
    QString      m_netAddress;
    QVariantList m_netAddresses;
    QString      m_netStatus;
    QString      m_pairCode;
    bool         m_pairActive = false;
    QVariantList m_pairedDevices;

    // ---- 投递 ----
    QVariantList m_outboxList;
    bool         m_flushBusy = false;
    bool         m_fetchBusy = false;

    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_inboxTimer = nullptr;
    QTimer *m_debounce = nullptr;
    QThread *m_receiveThread = nullptr;
    bool     m_pendingSync = false;
    qint64   m_manifestMtime = 0;
    qint64   m_manifestSize = 0;
};
