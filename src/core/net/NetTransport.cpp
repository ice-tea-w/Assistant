#include "core/net/NetTransport.h"

#include "core/net/DeviceAuth.h"
#include "core/net/Identity.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslConfiguration>

namespace Sync::Net {

namespace {

constexpr int    kPageLimit  = 200;
constexpr qint64 kChunkBytes = 64 * 1024;
constexpr int    kMaxPages   = 4096;   // 防对端 nextCursor 不前进导致死循环

QByteArray toJson(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QString errorText(const Frame &frame)
{
    const QJsonObject obj = QJsonDocument::fromJson(frame.payload).object();
    return QStringLiteral("%1：%2")
        .arg(obj.value(QStringLiteral("code")).toString(QStringLiteral("ERROR")),
             obj.value(QStringLiteral("message")).toString());
}

QString fileHash(const QString &path, bool *ok)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        return QString();
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    const bool hashed = hasher.addData(&file);
    *ok = hashed;
    return QString::fromLatin1(hasher.result().toHex());
}

} // namespace

NetTransport::NetTransport(NetPeer peer)
    : m_peer(std::move(peer))
{
}

NetTransport::~NetTransport()
{
    disconnectPeer();
}

QString NetTransport::name() const
{
    return QStringLiteral("net");
}

int NetTransport::priority() const
{
    return 0;   // 直连优先于共享目录
}

QString NetTransport::lastError() const
{
    return m_lastError;
}

bool NetTransport::fail(const QString &message)
{
    m_lastError = message;
    qWarning("NetTransport: %s", qPrintable(message));
    return false;
}

void NetTransport::disconnectPeer()
{
    m_authenticated = false;
    m_decoder.reset();
    if (m_socket.state() != QAbstractSocket::UnconnectedState)
        m_socket.abort();
}

bool NetTransport::ensureConnected()
{
    if (m_socket.state() == QAbstractSocket::ConnectedState && m_authenticated)
        return true;

    disconnectPeer();

    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    // 自签证书走不了系统的链校验，改成"连上之后手比指纹"
    cfg.setPeerVerifyMode(QSslSocket::QueryPeer);
    cfg.setProtocol(QSsl::TlsV1_2OrLater);
    m_socket.setSslConfiguration(cfg);

    m_socket.connectToHostEncrypted(m_peer.host, m_peer.port);
    if (!m_socket.waitForEncrypted(m_peer.timeoutMs))
        return fail(QStringLiteral("连接 %1:%2 失败：%3")
                        .arg(m_peer.host)
                        .arg(m_peer.port)
                        .arg(m_socket.errorString()));

    const QSslCertificate peerCert = m_socket.peerCertificate();
    if (peerCert.isNull())
        return fail(QStringLiteral("对端没有出示证书"));

    const QString fingerprint = fingerprintOf(peerCert);
    if (!m_peer.expectedFingerprint.isEmpty()
        && fingerprint.compare(m_peer.expectedFingerprint, Qt::CaseInsensitive) != 0) {
        disconnectPeer();
        return fail(QStringLiteral("对端证书指纹不匹配：期望 %1，实际 %2")
                        .arg(m_peer.expectedFingerprint, fingerprint));
    }

    const quint32 helloId = m_nextReqId++;
    QJsonObject hello;
    hello.insert(QStringLiteral("deviceId"), m_peer.deviceId);
    if (!sendFrame(FrameType::Hello, helloId, toJson(hello), quint16(FrameFlag::FlagJson))) {
        disconnectPeer();
        return false;
    }

    Frame challenge;
    if (!readFrame(&challenge, m_peer.timeoutMs)) {
        disconnectPeer();
        return false;
    }
    if (challenge.type == FrameType::Error) {
        const QString text = errorText(challenge);
        disconnectPeer();
        return fail(text);
    }
    if (challenge.type != FrameType::Challenge) {
        disconnectPeer();
        return fail(QStringLiteral("协议错误：Hello 之后期望 Challenge，收到 %1")
                        .arg(int(challenge.type)));
    }

    const QByteArray response = challengeResponse(m_peer.deviceSecret, challenge.payload,
                                                  m_peer.deviceId, fingerprint);
    if (!sendFrame(FrameType::Auth, m_nextReqId++, response, quint16(FrameFlag::FlagBlob))) {
        disconnectPeer();
        return false;
    }

    Frame ack;
    if (!readFrame(&ack, m_peer.timeoutMs)) {
        disconnectPeer();
        return false;
    }
    if (ack.type != FrameType::HelloAck) {
        const QString text = ack.type == FrameType::Error ? errorText(ack)
                                                          : QStringLiteral("认证未通过");
        disconnectPeer();
        return fail(text);
    }

    m_authenticated = true;
    m_lastError.clear();
    return true;
}

bool NetTransport::sendFrame(FrameType type, quint32 reqId, const QByteArray &payload, quint16 flags)
{
    if (m_socket.state() != QAbstractSocket::ConnectedState)
        return fail(QStringLiteral("连接已断开"));

    Frame frame;
    frame.type    = type;
    frame.flags   = flags;
    frame.reqId   = reqId;
    frame.payload = payload;

    const QByteArray bytes = encodeFrame(frame);
    if (m_socket.write(bytes) != bytes.size())
        return fail(QStringLiteral("发送失败：%1").arg(m_socket.errorString()));

    return m_socket.waitForBytesWritten(m_peer.timeoutMs);
}

bool NetTransport::readFrame(Frame *out, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();

    while (true) {
        Frame frame;
        QString error;
        const FrameDecoder::Status status = m_decoder.next(&frame, &error);

        if (status == FrameDecoder::Status::Frame) {
            *out = frame;
            return true;
        }
        if (status == FrameDecoder::Status::Error)
            return fail(QStringLiteral("协议错误：%1").arg(error));

        const int remain = timeoutMs - int(timer.elapsed());
        if (remain <= 0)
            return fail(QStringLiteral("等待对端响应超时"));

        if (!m_socket.waitForReadyRead(remain)) {
            if (m_socket.state() != QAbstractSocket::ConnectedState)
                return fail(QStringLiteral("对端断开连接：%1").arg(m_socket.errorString()));
            continue;
        }
        m_decoder.append(m_socket.readAll());
    }
}

bool NetTransport::requestJson(FrameType type, quint32 reqId, const QByteArray &payload, Frame *reply)
{
    if (!sendFrame(type, reqId, payload, quint16(FrameFlag::FlagJson)))
        return false;
    if (!readFrame(reply, m_peer.timeoutMs * 3))
        return false;
    if (reply->type == FrameType::Error)
        return fail(errorText(*reply));
    return true;
}

bool NetTransport::probe()
{
    return ensureConnected();
}

QList<RemoteFileInfo> NetTransport::listRemote()
{
    QList<RemoteFileInfo> out;
    m_aliases.clear();
    if (!ensureConnected())
        return out;

    int cursor = 0;
    for (int page = 0; page < kMaxPages; ++page) {
        QJsonObject request;
        request.insert(QStringLiteral("cursor"), cursor);
        request.insert(QStringLiteral("limit"), kPageLimit);

        Frame reply;
        if (!requestJson(FrameType::List, m_nextReqId++, toJson(request), &reply)) {
            disconnectPeer();
            return {};
        }
        if (reply.type != FrameType::ListResp)
            return (void(fail(QStringLiteral("协议错误：期望 ListResp"))), QList<RemoteFileInfo>());

        const QJsonObject obj = QJsonDocument::fromJson(reply.payload).object();
        for (const QJsonValue &value : obj.value(QStringLiteral("files")).toArray()) {
            const QJsonObject item = value.toObject();
            RemoteFileInfo info;
            info.relPath = item.value(QStringLiteral("path")).toString();
            info.size    = qint64(item.value(QStringLiteral("size")).toDouble(-1));
            info.mtime   = qint64(item.value(QStringLiteral("mtime")).toDouble(0));
            info.hash    = item.value(QStringLiteral("hash")).toString();
            if (info.relPath.isEmpty())
                continue;
            const QString alias = info.relPath.section(QLatin1Char('/'), 0, 0);
            if (!m_aliases.contains(alias))
                m_aliases.append(alias);
            out.append(info);
        }

        if (!obj.value(QStringLiteral("hasMore")).toBool())
            break;

        const int next = obj.value(QStringLiteral("nextCursor")).toInt();
        if (next <= cursor)
            break;   // 对端没有推进游标，别死循环
        cursor = next;
    }

    return out;
}

bool NetTransport::fetch(const QString &relPath, const QString &destPath)
{
    if (!ensureConnected())
        return false;

    QJsonObject request;
    request.insert(QStringLiteral("path"), relPath);

    const quint32 reqId = m_nextReqId++;
    if (!sendFrame(FrameType::Get, reqId, toJson(request), quint16(FrameFlag::FlagJson)))
        return false;

    const QString partPath = destPath + QStringLiteral(".assistant-part");
    QFile part(partPath);
    if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("无法写入 %1").arg(partPath));

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    bool ok = false;
    QByteArray expectedHash;
    while (true) {
        Frame frame;
        if (!readFrame(&frame, m_peer.timeoutMs * 6)) {
            part.close();
            QFile::remove(partPath);
            return false;
        }
        if (frame.reqId != reqId)
            continue;   // v1 串行，多余的帧直接丢弃

        if (frame.type == FrameType::Chunk) {
            if (part.write(frame.payload) != frame.payload.size()) {
                part.close();
                QFile::remove(partPath);
                return fail(QStringLiteral("写盘失败：%1").arg(destPath));
            }
            hasher.addData(frame.payload);
            continue;
        }
        if (frame.type == FrameType::End) {
            const QJsonObject obj = QJsonDocument::fromJson(frame.payload).object();
            expectedHash = obj.value(QStringLiteral("hash")).toString().toLatin1();
            ok = true;
            break;
        }
        if (frame.type == FrameType::Error) {
            const QString text = errorText(frame);
            part.close();
            QFile::remove(partPath);
            return fail(text);
        }
        part.close();
        QFile::remove(partPath);
        return fail(QStringLiteral("协议错误：Get 收到意外的帧 %1").arg(int(frame.type)));
    }

    part.close();

    const QByteArray actual = hasher.result().toHex();
    if (!expectedHash.isEmpty() && actual != expectedHash) {
        QFile::remove(partPath);
        return fail(QStringLiteral("校验失败：%1").arg(relPath));
    }
    if (!ok) {
        QFile::remove(partPath);
        return fail(QStringLiteral("传输未完成：%1").arg(relPath));
    }

    QFile::remove(destPath);
    if (!QFile::rename(partPath, destPath)) {
        QFile::remove(partPath);
        return fail(QStringLiteral("无法落盘：%1").arg(destPath));
    }
    return true;
}

bool NetTransport::push(const QString &localPath, const QString &relPath)
{
    if (!ensureConnected())
        return false;

    bool hashed = false;
    const QString hash = fileHash(localPath, &hashed);
    if (!hashed)
        return fail(QStringLiteral("无法读取本地文件：%1").arg(localPath));

    QFile src(localPath);
    if (!src.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法读取本地文件：%1").arg(localPath));

    QJsonObject put;
    put.insert(QStringLiteral("path"), relPath);
    put.insert(QStringLiteral("hash"), hash);
    put.insert(QStringLiteral("size"), double(src.size()));

    const quint32 reqId = m_nextReqId++;
    if (!sendFrame(FrameType::Put, reqId, toJson(put), quint16(FrameFlag::FlagJson)))
        return false;

    while (!src.atEnd()) {
        const QByteArray chunk = src.read(kChunkBytes);
        if (chunk.isEmpty())
            break;
        if (!sendFrame(FrameType::PutChunk, reqId, chunk, quint16(FrameFlag::FlagBlob)))
            return false;

        // 服务端可能在 Put 阶段就拒了（目录建不出来、路径不安全…）。
        // 不看就接着灌数据，只会让对端对每个块回一条 NO_PUT，最后还得清半天。
        if (m_socket.bytesAvailable() > 0) {
            m_decoder.append(m_socket.readAll());
            Frame early;
            QString error;
            while (m_decoder.next(&early, &error) == FrameDecoder::Status::Frame) {
                if (early.type == FrameType::Error)
                    return fail(errorText(early));
            }
            if (m_decoder.failed())
                return fail(QStringLiteral("协议错误：%1").arg(error));
        }
    }

    QJsonObject end;
    end.insert(QStringLiteral("hash"), hash);
    if (!sendFrame(FrameType::End, reqId, toJson(end), quint16(FrameFlag::FlagJson)))
        return false;

    Frame ack;
    while (true) {
        if (!readFrame(&ack, m_peer.timeoutMs * 6))
            return false;
        if (ack.type == FrameType::Error || ack.reqId == reqId)
            break;
    }
    if (ack.type == FrameType::Error)
        return fail(errorText(ack));
    if (ack.type != FrameType::PutAck)
        return fail(QStringLiteral("协议错误：期望 PutAck，收到 %1").arg(int(ack.type)));
    return true;
}

bool NetTransport::removeRemote(const QString &relPath)
{
    if (!ensureConnected())
        return false;

    QJsonObject request;
    request.insert(QStringLiteral("path"), relPath);

    Frame reply;
    if (!requestJson(FrameType::Delete, m_nextReqId++, toJson(request), &reply))
        return false;
    if (reply.type != FrameType::DeleteAck)
        return fail(QStringLiteral("协议错误：期望 DeleteAck，收到 %1").arg(int(reply.type)));
    return true;
}

QStringList NetTransport::remoteAliases() const
{
    return m_aliases;
}

} // namespace Sync::Net
