#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

#include "core/net/Identity.h"

class QSslServer;
class QSslSocket;
class QTimer;

namespace Sync::Net {

class TlsAcceptor;

/// 对端一个文件的元信息（协议层用，与 Sync::RemoteFileInfo 字段一致）。
struct ServedFile
{
    QString relPath;
    qint64  size  = 0;
    qint64  mtime = 0;
    QString hash;
};

/// 一个对外服务的根：relPath 的第一段是 alias，其余部分相对 localPath。
/// alias 为空表示"服务这一个根、relPath 不带前缀"（兼容旧的单共享根形态）。
struct ServedRoot
{
    QString localPath;
    QString alias;
};

/// ASYN v2 服务端（host 侧）。
///
/// 只负责协议与认证：文件读写都落在构造时给的一个共享根目录里，
/// 不认识 SyncEngine、不碰 sync.db。设备口令由调用方注入（`secretLookup`），
/// 这样单测不需要数据库，正式代码把 SyncDatabase::device() 包一层即可。
///
/// 线程：内部用 QSslServer + 事件循环，必须跑在有 exec() 的线程里；
/// 客户端是阻塞式的（见 NetTransport），所以服务端不能和客户端挤在同一个线程。
class SyncServer : public QObject
{
    Q_OBJECT

public:
    /// 返回 32 字节口令；返回空表示"不认识这台设备"，握手直接失败。
    using SecretLookup = std::function<QByteArray(const QString &deviceId)>;

    SyncServer(const Identity &identity, SecretLookup secretLookup, QObject *parent = nullptr);
    ~SyncServer() override;

    /// port = 0 时由系统分配，真实端口看 port()。
    /// sharedRoot 必须已存在且可写；其下每个一级目录就是一个同步别名。
    bool listen(quint16 port, const QString &sharedRoot);
    /// 多根形态：每个根的 alias 必须非空、唯一，relPath = alias + '/' + 相对路径。
    bool listen(quint16 port, const QList<ServedRoot> &roots);
    void close();

    quint16 port() const;
    QString lastError() const;
    QString sharedRoot() const;
    QList<ServedRoot> roots() const;

    /// 已认证成功的连接数（测试与界面用）。
    int readyConnections() const;

    /// 认证失败限速（设计 §4.3）：同一 IP 连续失败 maxFailures 次后锁 lockMs 毫秒。
    /// 默认 5 次 / 60 秒。测试把 lockMs 调小，才不用真等一分钟。
    void setAuthLockPolicy(int maxFailures, int lockMs);

    /// 配对成功后要落库的东西（回调在服务端线程里执行）。
    struct PairedDevice
    {
        QString    deviceId;
        QString    name;
        QString    fingerprint;   ///< 客户端自报的证书指纹，只作标识（Schannel 客户端不出示证书）
        QByteArray secret;        ///< 32 字节设备口令，此后用于挑战应答
        qint64     pairedAt = 0;
    };
    using PairSink = std::function<void(const PairedDevice &)>;

    /// 打开配对窗口并返回 6 位码：一次性，配对成功或超时即关闭（设计 §4.2）。
    QString openPairingWindow(int timeoutSeconds = 300);
    void closePairingWindow();
    bool pairingOpen() const;

    /// 配对成功时调用；host 侧把它接到 `SyncDatabase::upsertDevice()`。
    void setPairSink(PairSink sink);
    /// 一个窗口内允许输错几次配对码，默认 3。
    void setPairingAttemptLimit(int attempts);

signals:
    void clientAuthenticated(const QString &deviceId);
    void authFailed(const QString &deviceId);
    void devicePaired(const QString &deviceId);

private:
    class Connection;

    void onNewConnection();
    void onReadyRead(QSslSocket *socket);
    void onDisconnected(QSslSocket *socket);
    void adoptSocket(QSslSocket *socket);

    void handleFrame(Connection &conn, const struct Frame &frame);
    void handleHello(Connection &conn, const struct Frame &frame);
    void handleAuth(Connection &conn, const struct Frame &frame);
    void handlePairHello(Connection &conn, const struct Frame &frame);
    void handlePairProof(Connection &conn, const struct Frame &frame);
    void handleList(Connection &conn, const struct Frame &frame);
    void handleGet(Connection &conn, const struct Frame &frame);
    void handlePut(Connection &conn, const struct Frame &frame);
    void handlePutChunk(Connection &conn, const struct Frame &frame);
    void handlePutEnd(Connection &conn, const struct Frame &frame);
    void handleDelete(Connection &conn, const struct Frame &frame);
    void dropPartFile(Connection &conn);

    QList<ServedFile> collectFiles();
    QString hashOf(const QString &absPath);

    /// 把 relPath 拆成（根的绝对路径, 根内相对路径）。没有匹配的根返回 false。
    bool splitRel(const QString &relPath, QString *rootAbs, QString *subRel) const;

    bool isPeerLocked(const QString &peer) const;
    void recordAuthFailure(const QString &peer);
    void clearAuthFailures(const QString &peer);

    QString indexPath() const;
    void loadIndex();
    bool saveIndex();
    void markIndexDirty();

    void send(Connection &conn, quint8 type, quint32 reqId, const QByteArray &payload);
    void sendError(Connection &conn, quint32 reqId, const QString &code, const QString &message);

    struct CachedHash
    {
        qint64  size  = -1;
        qint64  mtime = 0;
        QString hash;
    };

    Identity                     m_identity;
    SecretLookup                 m_secretLookup;
    std::unique_ptr<TlsAcceptor> m_server;
    QList<ServedRoot>            m_roots;
    /// 第一个根的路径，仅用于兼容 `.sync/trash` 与 index.json 的落点。
    QString                      m_sharedRoot;
    QString                      m_lastError;
    QHash<QSslSocket *, std::shared_ptr<Connection>> m_connections;

    /// `.assistant-sync/index.json` 的内存镜像：relPath → size/mtime/hash。
    /// 命中条件是 size + mtime 都没变（与 FolderTransport 的 manifest 同一套失效规则）。
    QHash<QString, CachedHash>   m_index;
    bool                         m_indexDirty = false;
    QTimer                      *m_indexTimer = nullptr;

    struct FailureRecord
    {
        int    count         = 0;
        qint64 lockedUntilMs = 0;
    };
    QHash<QString, FailureRecord> m_failures;
    int                           m_maxAuthFailures = 5;
    int                           m_authLockMs      = 60 * 1000;

    /// 配对窗口：6 位码 + 截止时刻 + 已错次数（一次性，成功即清空）。
    QString m_pairCode;
    qint64  m_pairExpiresMs    = 0;
    int     m_pairAttempts     = 0;
    int     m_pairAttemptLimit = 3;
    PairSink m_pairSink;
};

} // namespace Sync::Net
