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
    void close();

    quint16 port() const;
    QString lastError() const;
    QString sharedRoot() const;

    /// 已认证成功的连接数（测试与界面用）。
    int readyConnections() const;

signals:
    void clientAuthenticated(const QString &deviceId);
    void authFailed(const QString &deviceId);

private:
    class Connection;

    void onNewConnection();
    void onReadyRead(QSslSocket *socket);
    void onDisconnected(QSslSocket *socket);
    void adoptSocket(QSslSocket *socket);

    void handleFrame(Connection &conn, const struct Frame &frame);
    void handleHello(Connection &conn, const struct Frame &frame);
    void handleAuth(Connection &conn, const struct Frame &frame);
    void handleList(Connection &conn, const struct Frame &frame);
    void handleGet(Connection &conn, const struct Frame &frame);
    void handlePut(Connection &conn, const struct Frame &frame);
    void handlePutChunk(Connection &conn, const struct Frame &frame);
    void handlePutEnd(Connection &conn, const struct Frame &frame);
    void handleDelete(Connection &conn, const struct Frame &frame);
    void dropPartFile(Connection &conn);

    QList<ServedFile> collectFiles() const;
    QString hashOf(const QString &absPath, qint64 size, qint64 mtime);

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
    QString                      m_sharedRoot;
    QString                      m_lastError;
    QHash<QSslSocket *, std::shared_ptr<Connection>> m_connections;
    QHash<QString, CachedHash>   m_hashCache;
};

} // namespace Sync::Net
