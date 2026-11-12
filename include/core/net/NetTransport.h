#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QSslSocket>
#include <QString>
#include <QStringList>

#include <memory>

#include "core/net/SyncFrame.h"
#include "core/sync/ITransport.h"

namespace Sync::Net {

/// 直连通道的对端地址与凭据。
struct NetPeer
{
    QString    host;
    quint16    port = 45871;
    QString    expectedFingerprint;   // 对端证书 SHA-256，小写 hex；不匹配即拒绝连接
    QString    deviceId;              // 本机在服务端的身份
    QByteArray deviceSecret;          // 配对时交换的 32 字节口令
    int        timeoutMs = 10000;
};

/// ASYN v2 客户端，实现 Sync::ITransport。
///
/// 阻塞式：用 waitForConnected / waitForEncrypted / waitForReadyRead 推进，
/// 因此**必须**跑在没有 GUI 事件循环的线程里（SyncBridge 的工作线程正是这样），
/// 而且服务端必须活在另一个线程里，否则两边互相等事件。设计 §5.4。
class NetTransport : public Sync::ITransport
{
public:
    explicit NetTransport(NetPeer peer);
    ~NetTransport() override;

    QString name() const override;
    int priority() const override;

    bool probe() override;
    QList<Sync::RemoteFileInfo> listRemote() override;
    bool fetch(const QString &relPath, const QString &destPath) override;
    bool push(const QString &localPath, const QString &relPath) override;
    bool removeRemote(const QString &relPath) override;
    QStringList remoteAliases() const override;

    QString lastError() const;

private:
    bool ensureConnected();
    void disconnectPeer();

    bool sendFrame(FrameType type, quint32 reqId, const QByteArray &payload, quint16 flags);
    bool readFrame(Frame *out, int timeoutMs);
    bool requestJson(FrameType type, quint32 reqId, const QByteArray &payload, Frame *reply);
    bool fail(const QString &message);

    NetPeer      m_peer;
    QSslSocket   m_socket;
    FrameDecoder m_decoder;
    QString      m_lastError;
    QStringList  m_aliases;
    quint32      m_nextReqId = 1;
    bool         m_authenticated = false;
};

} // namespace Sync::Net
