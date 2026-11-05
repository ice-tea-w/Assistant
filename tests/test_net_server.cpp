#include "core/net/DeviceAuth.h"
#include "core/net/Identity.h"
#include "core/net/SyncFrame.h"
#include "core/net/SyncServer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace Sync::Net;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(content);
}

/// 会自己泵事件循环的最小客户端：服务端与客户端同线程，不能阻塞等。
class TestClient
{
public:
    bool connectTo(quint16 port, const Identity &identity, int timeoutMs = 5000)
    {
        QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
        cfg.setPeerVerifyMode(QSslSocket::QueryPeer);
        cfg.setProtocol(QSsl::TlsV1_2OrLater);
        cfg.setLocalCertificate(identity.certificate);
        cfg.setPrivateKey(identity.privateKey);
        m_socket.setSslConfiguration(cfg);

        m_socket.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port);
        return pumpUntil([this] { return m_socket.isEncrypted(); }, timeoutMs);
    }

    bool send(const Frame &frame)
    {
        return m_socket.write(encodeFrame(frame)) > 0;
    }

    bool read(Frame *out, int timeoutMs = 5000)
    {
        const bool ok = pumpUntil([this] {
            Frame frame;
            QString error;
            const FrameDecoder::Status status = m_decoder.next(&frame, &error);
            if (status == FrameDecoder::Status::Frame) {
                m_pending = frame;
                m_hasPending = true;
                return true;
            }
            return false;
        }, timeoutMs);
        if (!ok)
            return false;
        *out = m_pending;
        m_hasPending = false;
        return true;
    }

    QSslSocket &socket() { return m_socket; }

private:
    template <typename Predicate>
    bool pumpUntil(Predicate pred, int timeoutMs)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            m_decoder.append(m_socket.readAll());
            if (pred())
                return true;
            if (m_socket.state() == QAbstractSocket::UnconnectedState && !m_socket.isEncrypted())
                return false;
            QTest::qWait(5);
        }
        return false;
    }

    QSslSocket   m_socket;
    FrameDecoder m_decoder;
    Frame        m_pending;
    bool         m_hasPending = false;
};

Frame makeJsonFrame(FrameType type, quint32 reqId, const QJsonObject &obj)
{
    Frame frame;
    frame.type    = type;
    frame.flags   = quint16(FrameFlag::FlagJson);
    frame.reqId   = reqId;
    frame.payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return frame;
}

} // namespace

class TestNetServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void listBeforeAuthIsRejected();
    void authThenListPages();
    void unknownDeviceIsRejected();
    void pingPongRoundTrip();

private:
    bool startServer(const QString &root);
    Identity makeIdentity(const QString &name);

    QTemporaryDir m_dir;
    Identity      m_serverIdentity;
    Identity      m_clientIdentity;
    QByteArray    m_deviceSecret;
};

void TestNetServer::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_serverIdentity = makeIdentity(QStringLiteral("server"));
    QVERIFY2(m_serverIdentity.isValid(), qPrintable(m_serverIdentity.error));
    m_clientIdentity = makeIdentity(QStringLiteral("client"));
    QVERIFY2(m_clientIdentity.isValid(), qPrintable(m_clientIdentity.error));
    m_deviceSecret = QByteArray(32, '\x5a');
}

Identity TestNetServer::makeIdentity(const QString &name)
{
    return loadOrCreateIdentity(m_dir.path() + QStringLiteral("/identity-") + name, name);
}

bool TestNetServer::startServer(const QString &root)
{
    QDir().mkpath(root);
    writeFile(root + QStringLiteral("/docs/a.txt"), "aaa");
    writeFile(root + QStringLiteral("/docs/b.txt"), "bbbb");
    writeFile(root + QStringLiteral("/照片/c.jpg"), "ccccc");
    writeFile(root + QStringLiteral("/readme.md"), "hello");
    return true;
}

void TestNetServer::listBeforeAuthIsRejected()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(startServer(root.path()));

    auto secret = [this](const QString &deviceId) {
        return deviceId == QStringLiteral("dev-test") ? m_deviceSecret : QByteArray();
    };
    SyncServer server(m_serverIdentity, secret);
    QVERIFY2(server.listen(0, root.path()), qPrintable(server.lastError()));
    QVERIFY(server.port() > 0);

    TestClient client;
    QVERIFY(client.connectTo(server.port(), m_clientIdentity));

    // 没 Hello 直接 List → 服务端必须拒绝并断开
    QJsonObject listReq;
    listReq.insert(QStringLiteral("cursor"), 0);
    QVERIFY(client.send(makeJsonFrame(FrameType::List, 1, listReq)));

    Frame reply;
    QVERIFY(client.read(&reply));
    QCOMPARE(reply.type, FrameType::Error);
    const QJsonObject error = QJsonDocument::fromJson(reply.payload).object();
    QCOMPARE(error.value(QStringLiteral("code")).toString(), QStringLiteral("EXPECTED_HELLO"));
    QCOMPARE(server.readyConnections(), 0);
}

void TestNetServer::authThenListPages()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(startServer(root.path()));

    auto secret = [this](const QString &deviceId) {
        return deviceId == QStringLiteral("dev-test") ? m_deviceSecret : QByteArray();
    };
    SyncServer server(m_serverIdentity, secret);
    QVERIFY2(server.listen(0, root.path()), qPrintable(server.lastError()));

    QSignalSpy authenticated(&server, &SyncServer::clientAuthenticated);

    TestClient client;
    QVERIFY(client.connectTo(server.port(), m_clientIdentity));

    QJsonObject hello;
    hello.insert(QStringLiteral("deviceId"), QStringLiteral("dev-test"));
    QVERIFY(client.send(makeJsonFrame(FrameType::Hello, 1, hello)));

    Frame challenge;
    QVERIFY(client.read(&challenge));
    QCOMPARE(challenge.type, FrameType::Challenge);
    QCOMPARE(challenge.payload.size(), kChallengeSize);

    const QByteArray response =
        challengeResponse(m_deviceSecret, challenge.payload, QStringLiteral("dev-test"),
                          m_serverIdentity.fingerprint);

    Frame auth;
    auth.type    = FrameType::Auth;
    auth.flags   = quint16(FrameFlag::FlagBlob);
    auth.reqId   = 2;
    auth.payload = response;
    QVERIFY(client.send(auth));

    Frame ack;
    QVERIFY(client.read(&ack));
    QCOMPARE(ack.type, FrameType::HelloAck);
    const QJsonObject ackObj = QJsonDocument::fromJson(ack.payload).object();
    QCOMPARE(ackObj.value(QStringLiteral("serverFingerprint")).toString(),
             m_serverIdentity.fingerprint);
    QCOMPARE(authenticated.count(), 1);
    QCOMPARE(server.readyConnections(), 1);

    // 第一页：limit=2
    QJsonObject page1Req;
    page1Req.insert(QStringLiteral("cursor"), 0);
    page1Req.insert(QStringLiteral("limit"), 2);
    QVERIFY(client.send(makeJsonFrame(FrameType::List, 3, page1Req)));

    Frame page1;
    QVERIFY(client.read(&page1));
    QCOMPARE(page1.type, FrameType::ListResp);
    QJsonObject page1Obj = QJsonDocument::fromJson(page1.payload).object();
    QCOMPARE(page1Obj.value(QStringLiteral("files")).toArray().size(), 2);
    QCOMPARE(page1Obj.value(QStringLiteral("hasMore")).toBool(), true);
    const int nextCursor = page1Obj.value(QStringLiteral("nextCursor")).toInt();

    QJsonObject page2Req;
    page2Req.insert(QStringLiteral("cursor"), nextCursor);
    page2Req.insert(QStringLiteral("limit"), 2);
    QVERIFY(client.send(makeJsonFrame(FrameType::List, 4, page2Req)));

    Frame page2;
    QVERIFY(client.read(&page2));
    QJsonObject page2Obj = QJsonDocument::fromJson(page2.payload).object();
    QCOMPARE(page2Obj.value(QStringLiteral("files")).toArray().size(), 2);
    QCOMPARE(page2Obj.value(QStringLiteral("hasMore")).toBool(), false);

    // 两页拼起来 = 完整清单（relPath 带别名前缀，按字典序）
    QStringList paths;
    for (const QJsonValue &v : page1Obj.value(QStringLiteral("files")).toArray())
        paths << v.toObject().value(QStringLiteral("path")).toString();
    for (const QJsonValue &v : page2Obj.value(QStringLiteral("files")).toArray())
        paths << v.toObject().value(QStringLiteral("path")).toString();

    QCOMPARE(paths.size(), 4);
    QVERIFY2(paths.contains(QStringLiteral("docs/a.txt")), qPrintable(paths.join(',')));
    QVERIFY2(paths.contains(QStringLiteral("照片/c.jpg")), qPrintable(paths.join(',')));
    QVERIFY2(paths.contains(QStringLiteral("readme.md")), qPrintable(paths.join(',')));

    // 哈希随清单一起给，且与文件内容一致
    const QString expectedHash =
        QString::fromLatin1(QCryptographicHash::hash(QByteArray("aaa"), QCryptographicHash::Sha256)
                                .toHex());
    for (const QJsonValue &v : page1Obj.value(QStringLiteral("files")).toArray()) {
        const QJsonObject item = v.toObject();
        if (item.value(QStringLiteral("path")).toString() == QStringLiteral("docs/a.txt"))
            QCOMPARE(item.value(QStringLiteral("hash")).toString(), expectedHash);
    }
}

void TestNetServer::unknownDeviceIsRejected()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(startServer(root.path()));

    auto secret = [](const QString &) { return QByteArray(); };   // 谁都不认识
    SyncServer server(m_serverIdentity, secret);
    QVERIFY2(server.listen(0, root.path()), qPrintable(server.lastError()));

    QSignalSpy failed(&server, &SyncServer::authFailed);

    TestClient client;
    QVERIFY(client.connectTo(server.port(), m_clientIdentity));

    QJsonObject hello;
    hello.insert(QStringLiteral("deviceId"), QStringLiteral("dev-unknown"));
    QVERIFY(client.send(makeJsonFrame(FrameType::Hello, 1, hello)));

    Frame challenge;
    QVERIFY(client.read(&challenge));
    QCOMPARE(challenge.type, FrameType::Challenge);

    Frame auth;
    auth.type    = FrameType::Auth;
    auth.flags   = quint16(FrameFlag::FlagBlob);
    auth.reqId   = 2;
    auth.payload = QByteArray(kChallengeSize, '\x01');
    QVERIFY(client.send(auth));

    Frame reply;
    QVERIFY(client.read(&reply));
    QCOMPARE(reply.type, FrameType::Error);
    QCOMPARE(QJsonDocument::fromJson(reply.payload).object()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("AUTH_FAILED"));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(server.readyConnections(), 0);
}

void TestNetServer::pingPongRoundTrip()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(startServer(root.path()));

    auto secret = [this](const QString &deviceId) {
        return deviceId == QStringLiteral("dev-test") ? m_deviceSecret : QByteArray();
    };
    SyncServer server(m_serverIdentity, secret);
    QVERIFY2(server.listen(0, root.path()), qPrintable(server.lastError()));

    TestClient client;
    QVERIFY(client.connectTo(server.port(), m_clientIdentity));

    QJsonObject hello;
    hello.insert(QStringLiteral("deviceId"), QStringLiteral("dev-test"));
    QVERIFY(client.send(makeJsonFrame(FrameType::Hello, 1, hello)));
    Frame challenge;
    QVERIFY(client.read(&challenge));

    Frame auth;
    auth.type    = FrameType::Auth;
    auth.flags   = quint16(FrameFlag::FlagBlob);
    auth.reqId   = 2;
    auth.payload = challengeResponse(m_deviceSecret, challenge.payload,
                                     QStringLiteral("dev-test"), m_serverIdentity.fingerprint);
    QVERIFY(client.send(auth));
    Frame ack;
    QVERIFY(client.read(&ack));
    QCOMPARE(ack.type, FrameType::HelloAck);

    Frame ping;
    ping.type  = FrameType::Ping;
    ping.flags = 0;
    ping.reqId = 9;
    QVERIFY(client.send(ping));

    Frame pong;
    QVERIFY(client.read(&pong));
    QCOMPARE(pong.type, FrameType::Pong);
    QCOMPARE(pong.reqId, quint32(9));
}

QTEST_MAIN(TestNetServer)
#include "test_net_server.moc"
