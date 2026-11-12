#include "core/net/PairingClient.h"

#include "core/net/DeviceAuth.h"
#include "core/net/Identity.h"
#include "core/net/SyncFrame.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QSslSocket>

namespace Sync::Net {
namespace {

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

bool sendFrame(QSslSocket &socket, int timeoutMs, FrameType type, quint32 reqId,
               const QByteArray &payload, quint16 flags, QString *error)
{
    Frame frame;
    frame.type    = type;
    frame.flags   = flags;
    frame.reqId   = reqId;
    frame.payload = payload;

    const QByteArray bytes = encodeFrame(frame);
    if (socket.write(bytes) != bytes.size()) {
        *error = QStringLiteral("发送失败：%1").arg(socket.errorString());
        return false;
    }
    if (!socket.waitForBytesWritten(timeoutMs)) {
        *error = QStringLiteral("发送超时：%1").arg(socket.errorString());
        return false;
    }
    return true;
}

bool readFrame(QSslSocket &socket, FrameDecoder &decoder, int timeoutMs, Frame *out,
               QString *error)
{
    QElapsedTimer timer;
    timer.start();

    while (true) {
        QString decodeError;
        const FrameDecoder::Status status = decoder.next(out, &decodeError);
        if (status == FrameDecoder::Status::Frame)
            return true;
        if (status == FrameDecoder::Status::Error) {
            *error = QStringLiteral("协议错误：%1").arg(decodeError);
            return false;
        }

        const int remain = timeoutMs - int(timer.elapsed());
        if (remain <= 0) {
            *error = QStringLiteral("等待对端响应超时");
            return false;
        }
        if (!socket.waitForReadyRead(remain)) {
            *error = QStringLiteral("等待对端响应超时：%1").arg(socket.errorString());
            return false;
        }
        decoder.append(socket.readAll());
    }
}

} // namespace

PairingOutcome pairWithHost(const NetPeer &peer, const QString &code,
                            const QString &clientName, const QString &clientFingerprint)
{
    PairingOutcome outcome;
    const int timeoutMs = peer.timeoutMs > 0 ? peer.timeoutMs : 10000;

    // PairHello 必须带自己的标识；调用方漏填时在这里直接报，免得白跑一趟服务端
    if (peer.deviceId.isEmpty()) {
        outcome.error = QStringLiteral("本机 deviceId 为空，无法发起配对");
        return outcome;
    }

    QSslSocket socket;
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(QSslSocket::QueryPeer);   // 自签证书：连上之后手比指纹
    cfg.setProtocol(QSsl::TlsV1_2OrLater);
    socket.setSslConfiguration(cfg);

    socket.connectToHostEncrypted(peer.host, peer.port);
    if (!socket.waitForEncrypted(timeoutMs)) {
        outcome.error = QStringLiteral("连接 %1:%2 失败：%3")
                            .arg(peer.host)
                            .arg(peer.port)
                            .arg(socket.errorString());
        return outcome;
    }

    const QSslCertificate peerCert = socket.peerCertificate();
    if (peerCert.isNull()) {
        outcome.error = QStringLiteral("对端没有出示证书");
        return outcome;
    }

    const QString serverFingerprint = fingerprintOf(peerCert);
    if (!peer.expectedFingerprint.isEmpty()
        && serverFingerprint.compare(peer.expectedFingerprint, Qt::CaseInsensitive) != 0) {
        outcome.error = QStringLiteral("对端证书指纹不匹配：期望 %1，实际 %2")
                            .arg(peer.expectedFingerprint, serverFingerprint);
        return outcome;
    }

    FrameDecoder decoder;
    quint32 nextReqId = 1;

    QJsonObject hello;
    hello.insert(QStringLiteral("deviceId"), peer.deviceId);
    hello.insert(QStringLiteral("name"), clientName);
    hello.insert(QStringLiteral("fingerprint"), clientFingerprint);
    if (!sendFrame(socket, timeoutMs, FrameType::PairHello, nextReqId++, toJson(hello),
                   quint16(FrameFlag::FlagJson), &outcome.error))
        return outcome;

    Frame challenge;
    if (!readFrame(socket, decoder, timeoutMs, &challenge, &outcome.error))
        return outcome;
    if (challenge.type == FrameType::Error) {
        outcome.error = errorText(challenge);
        return outcome;
    }
    if (challenge.type != FrameType::PairChallenge) {
        outcome.error = QStringLiteral("协议错误：期望 PairChallenge，收到 %1")
                            .arg(int(challenge.type));
        return outcome;
    }

    const QByteArray proof =
        pairingProof(code, challenge.payload, serverFingerprint, clientFingerprint);
    if (!sendFrame(socket, timeoutMs, FrameType::PairProof, nextReqId++, proof,
                   quint16(FrameFlag::FlagBlob), &outcome.error))
        return outcome;

    Frame result;
    if (!readFrame(socket, decoder, timeoutMs, &result, &outcome.error))
        return outcome;
    if (result.type == FrameType::Error) {
        outcome.error = errorText(result);
        return outcome;
    }
    if (result.type != FrameType::PairResult) {
        outcome.error = QStringLiteral("协议错误：期望 PairResult，收到 %1")
                            .arg(int(result.type));
        return outcome;
    }

    const QJsonObject obj = QJsonDocument::fromJson(result.payload).object();
    const QByteArray secret =
        QByteArray::fromBase64(obj.value(QStringLiteral("secret")).toString().toLatin1());
    if (secret.size() != kDeviceSecretSize) {
        outcome.error = QStringLiteral("主机返回的设备口令长度不对（%1 字节）")
                            .arg(secret.size());
        return outcome;
    }

    outcome.ok                = true;
    outcome.deviceId          = obj.value(QStringLiteral("deviceId")).toString();
    outcome.serverFingerprint = obj.value(QStringLiteral("serverFingerprint")).toString();
    outcome.deviceSecret      = secret;
    if (outcome.serverFingerprint.isEmpty())
        outcome.serverFingerprint = serverFingerprint;

    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState)
        socket.waitForDisconnected(1000);
    return outcome;
}

} // namespace Sync::Net
