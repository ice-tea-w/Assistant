#include <QElapsedTimer>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "core/net/DeviceAuth.h"
#include "core/net/Identity.h"
#include "core/net/SyncFrame.h"

/// 证明 Schannel 后端能做服务端 TLS、客户端能按指纹认服务端，
/// 以及"单向 TLS + 挑战应答"这套客户端认证在真实连接上跑得通。
class TestNetTls : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void serverListens();
    void handshakeExposesServerFingerprintToClient();
    void serverDoesNotRequestClientCertificate();
    void challengeResponseOverTls();

private:
    static QSslConfiguration configurationFor(const Sync::Net::Identity &identity);
    bool startPair(QSslServer &server, QSslSocket &client);

    std::unique_ptr<QTemporaryDir> m_dir;
    Sync::Net::Identity m_serverIdentity;
    Sync::Net::Identity m_clientIdentity;
    QSslSocket *m_accepted = nullptr;
};

QSslConfiguration TestNetTls::configurationFor(const Sync::Net::Identity &identity)
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificate(identity.certificate);
    configuration.setPrivateKey(identity.privateKey);
    // 自签证书过不了链校验，身份由指纹固定负责（设计 §4.4）。
    configuration.setPeerVerifyMode(QSslSocket::QueryPeer);
    configuration.setProtocol(QSsl::TlsV1_2OrLater);
    return configuration;
}

void TestNetTls::initTestCase()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    m_serverIdentity = Sync::Net::loadOrCreateIdentity(
        m_dir->filePath(QStringLiteral("server")), QStringLiteral("Assistant Server"));
    QVERIFY2(m_serverIdentity.error.isEmpty(), qPrintable(m_serverIdentity.error));

    m_clientIdentity = Sync::Net::loadOrCreateIdentity(
        m_dir->filePath(QStringLiteral("client")), QStringLiteral("Assistant Client"));
    QVERIFY2(m_clientIdentity.error.isEmpty(), qPrintable(m_clientIdentity.error));
}

bool TestNetTls::startPair(QSslServer &server, QSslSocket &client)
{
    m_accepted = nullptr;
    server.setSslConfiguration(configurationFor(m_serverIdentity));
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        qWarning() << "listen failed:" << server.errorString();
        return false;
    }

    client.setSslConfiguration(configurationFor(m_clientIdentity));
    client.connectToHostEncrypted(QStringLiteral("127.0.0.1"), server.serverPort());

    if (!QTest::qWaitFor([&] { return server.hasPendingConnections(); }, 10000)) {
        qWarning() << "no pending connection:" << client.errorString();
        return false;
    }
    m_accepted = qobject_cast<QSslSocket *>(server.nextPendingConnection());
    if (!m_accepted) {
        qWarning() << "accepted socket is not a QSslSocket";
        return false;
    }

    if (!QTest::qWaitFor([&] { return m_accepted->isEncrypted(); }, 10000)) {
        qWarning() << "server side not encrypted:" << m_accepted->errorString();
        return false;
    }
    if (!QTest::qWaitFor([&] { return client.isEncrypted(); }, 10000)) {
        qWarning() << "client side not encrypted:" << client.errorString();
        return false;
    }
    return true;
}

void TestNetTls::serverListens()
{
    QSslServer server;
    server.setSslConfiguration(configurationFor(m_serverIdentity));
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QVERIFY(server.isListening());
    QVERIFY(server.serverPort() != 0);
}

void TestNetTls::handshakeExposesServerFingerprintToClient()
{
    QSslServer server;
    QSslSocket client;
    QVERIFY(startPair(server, client));

    QVERIFY(!client.peerCertificate().isNull());
    QCOMPARE(Sync::Net::fingerprintOf(client.peerCertificate()), m_serverIdentity.fingerprint);
}

/// Schannel 后端在 QueryPeer 下不会向客户端索要证书
/// （qtls_schannel.cpp 的 getContextRequirements 只对 VerifyPeer 设 ASC_REQ_MUTUAL_AUTH，
/// 源码注释："There doesn't seem to be a way to ask for an optional client cert"）。
/// 所以客户端身份必须靠应用层的挑战应答，不能指望 mTLS。
void TestNetTls::serverDoesNotRequestClientCertificate()
{
    QSslServer server;
    QSslSocket client;
    QVERIFY(startPair(server, client));

    QVERIFY(!m_accepted->isEncrypted() || m_accepted->peerCertificate().isNull());
    QCOMPARE(m_accepted->sslConfiguration().peerVerifyMode(), QSslSocket::QueryPeer);
}

void TestNetTls::challengeResponseOverTls()
{
    QSslServer server;
    QSslSocket client;
    QVERIFY(startPair(server, client));

    const QByteArray deviceSecret = Sync::Net::randomBytes(Sync::Net::kDeviceSecretSize);
    const QString deviceId = QStringLiteral("device-a");

    Sync::Net::Frame challenge;
    challenge.type = Sync::Net::FrameType::Challenge;
    challenge.reqId = 1;
    challenge.flags = Sync::Net::FlagBlob;
    challenge.payload = Sync::Net::randomBytes(Sync::Net::kChallengeSize);
    m_accepted->write(Sync::Net::encodeFrame(challenge));
    QVERIFY(QTest::qWaitFor([&] { return client.bytesAvailable() > 0; }, 10000));

    Sync::Net::FrameDecoder clientDecoder;
    clientDecoder.append(client.readAll());
    Sync::Net::Frame receivedChallenge;
    QCOMPARE(clientDecoder.next(&receivedChallenge), Sync::Net::FrameDecoder::Status::Frame);
    QCOMPARE(receivedChallenge.type, Sync::Net::FrameType::Challenge);
    QCOMPARE(receivedChallenge.reqId, quint32(1));

    const QString serverFingerprint = Sync::Net::fingerprintOf(client.peerCertificate());
    QCOMPARE(serverFingerprint, m_serverIdentity.fingerprint);

    Sync::Net::Frame auth;
    auth.type = Sync::Net::FrameType::Auth;
    auth.reqId = 1;
    auth.flags = Sync::Net::FlagBlob;
    auth.payload = Sync::Net::challengeResponse(deviceSecret, receivedChallenge.payload, deviceId,
                                                serverFingerprint);
    client.write(Sync::Net::encodeFrame(auth));
    QVERIFY(QTest::qWaitFor([&] { return client.bytesToWrite() == 0; }, 10000));

    QVERIFY(QTest::qWaitFor([&] { return m_accepted->bytesAvailable() > 0; }, 10000));
    Sync::Net::FrameDecoder serverDecoder;
    serverDecoder.append(m_accepted->readAll());
    Sync::Net::Frame receivedAuth;
    QCOMPARE(serverDecoder.next(&receivedAuth), Sync::Net::FrameDecoder::Status::Frame);
    QCOMPARE(receivedAuth.type, Sync::Net::FrameType::Auth);

    const QByteArray expected = Sync::Net::challengeResponse(deviceSecret, challenge.payload, deviceId,
                                                             m_serverIdentity.fingerprint);
    QVERIFY(Sync::Net::verifyChallengeResponse(expected, receivedAuth.payload));

    // 绑定服务端指纹：换成客户端自己的指纹就验不过。
    const QByteArray wrong = Sync::Net::challengeResponse(deviceSecret, challenge.payload, deviceId,
                                                          m_clientIdentity.fingerprint);
    QVERIFY(!Sync::Net::verifyChallengeResponse(expected, wrong));
}

QTEST_MAIN(TestNetTls)
#include "test_net_tls.moc"
