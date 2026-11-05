#include "core/net/DeviceAuth.h"
#include "core/net/Identity.h"
#include "core/net/SyncFrame.h"
#include "core/net/SyncServer.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
    void authFailuresLockPeer();
    void indexIsUsedAcrossRestart();

private:
    bool startServer(const QString &root);
    Identity makeIdentity(const QString &name);
    bool authenticate(TestClient &client, quint32 baseId);
    QHash<QString, QString> listAll(SyncServer &server, bool *ok);

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

bool TestNetServer::authenticate(TestClient &client, quint32 baseId)
{
    QJsonObject hello;
    hello.insert(QStringLiteral("deviceId"), QStringLiteral("dev-test"));
    if (!client.send(makeJsonFrame(FrameType::Hello, baseId, hello)))
        return false;

    Frame challenge;
    if (!client.read(&challenge) || challenge.type != FrameType::Challenge)
        return false;

    Frame auth;
    auth.type    = FrameType::Auth;
    auth.flags   = quint16(FrameFlag::FlagBlob);
    auth.reqId   = baseId + 1;
    auth.payload = challengeResponse(m_deviceSecret, challenge.payload,
                                     QStringLiteral("dev-test"), m_serverIdentity.fingerprint);
    if (!client.send(auth))
        return false;

    Frame ack;
    return client.read(&ack) && ack.type == FrameType::HelloAck;
}

QHash<QString, QString> TestNetServer::listAll(SyncServer &server, bool *ok)
{
    QHash<QString, QString> result;
    *ok = false;

    TestClient client;
    if (!client.connectTo(server.port(), m_clientIdentity))
        return result;
    if (!authenticate(client, 100))
        return result;

    int cursor = 0;
    quint32 reqId = 200;
    while (true) {
        QJsonObject req;
        req.insert(QStringLiteral("cursor"), cursor);
        req.insert(QStringLiteral("limit"), 2);
        if (!client.send(makeJsonFrame(FrameType::List, reqId++, req)))
            return result;

        Frame reply;
        if (!client.read(&reply) || reply.type != FrameType::ListResp)
            return result;

        const QJsonObject obj = QJsonDocument::fromJson(reply.payload).object();
        for (const QJsonValue &v : obj.value(QStringLiteral("files")).toArray()) {
            const QJsonObject item = v.toObject();
            result.insert(item.value(QStringLiteral("path")).toString(),
                          item.value(QStringLiteral("hash")).toString());
        }
        if (!obj.value(QStringLiteral("hasMore")).toBool())
            break;
        cursor = obj.value(QStringLiteral("nextCursor")).toInt();
    }

    *ok = true;
    return result;
}

void TestNetServer::authFailuresLockPeer()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(startServer(root.path()));

    auto secret = [this](const QString &deviceId) {
        return deviceId == QStringLiteral("dev-test") ? m_deviceSecret : QByteArray();
    };
    SyncServer server(m_serverIdentity, secret);
    server.setAuthLockPolicy(2, 60 * 1000);   // 两次就锁，免得等一分钟
    QVERIFY2(server.listen(0, root.path()), qPrintable(server.lastError()));

    auto badAuth = [&](quint32 reqId) {
        TestClient client;
        if (!client.connectTo(server.port(), m_clientIdentity))
            return QStringLiteral("<connect-failed>");

        QJsonObject hello;
        hello.insert(QStringLiteral("deviceId"), QStringLiteral("dev-test"));
        if (!client.send(makeJsonFrame(FrameType::Hello, reqId, hello)))
            return QStringLiteral("<send-failed>");

        // 锁定期内服务端连 Challenge 都不给，第一帧就回 AUTH_LOCKED
        Frame reply;
        if (!client.read(&reply))
            return QStringLiteral("<no-reply>");
        if (reply.type == FrameType::Error)
            return QJsonDocument::fromJson(reply.payload).object()
                .value(QStringLiteral("code")).toString();
        if (reply.type != FrameType::Challenge)
            return QStringLiteral("<type-%1>").arg(int(reply.type));

        Frame auth;
        auth.type    = FrameType::Auth;
        auth.flags   = quint16(FrameFlag::FlagBlob);
        auth.reqId   = reqId + 1;
        auth.payload = QByteArray(kChallengeSize, '\x00');
        if (!client.send(auth))
            return QStringLiteral("<send-failed>");

        Frame final;
        if (!client.read(&final))
            return QStringLiteral("<no-reply>");
        if (final.type != FrameType::Error)
            return QStringLiteral("<type-%1>").arg(int(final.type));
        return QJsonDocument::fromJson(final.payload).object()
            .value(QStringLiteral("code")).toString();
    };

    QCOMPARE(badAuth(1), QStringLiteral("AUTH_FAILED"));
    QCOMPARE(badAuth(10), QStringLiteral("AUTH_FAILED"));
    // 第三次连 Challenge 都不给了：锁定期内首帧就回 AUTH_LOCKED
    QCOMPARE(badAuth(20), QStringLiteral("AUTH_LOCKED"));
}

void TestNetServer::indexIsUsedAcrossRestart()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(startServer(root.path()));

    auto secret = [this](const QString &deviceId) {
        return deviceId == QStringLiteral("dev-test") ? m_deviceSecret : QByteArray();
    };
    const QString indexPath = root.path() + QStringLiteral("/.assistant-sync/index.json");

    bool ok = false;
    QHash<QString, QString> first;
    {
        SyncServer server(m_serverIdentity, secret);
        QVERIFY2(server.listen(0, root.path()), qPrintable(server.lastError()));
        first = listAll(server, &ok);
    }
    QVERIFY(ok);
    QVERIFY(QFile::exists(indexPath));
    QCOMPARE(first.value(QStringLiteral("docs/a.txt")),
             QString::fromLatin1(QCryptographicHash::hash(QByteArray("aaa"),
                                                          QCryptographicHash::Sha256).toHex()));

    // 索引的失效条件是 size + mtime（与 FolderTransport 的 manifest 同一套）：
    // 这里故意改内容但保持长度并把 mtime 掰回去，第二次 List 必须给出**索引里的旧哈希**——
    // 这正是"命中"的证明，也是这套规则已知的盲区。
    const QString filePath = root.path() + QStringLiteral("/docs/a.txt");
    const QDateTime originalMtime = QFileInfo(filePath).lastModified();
    writeFile(filePath, "bbb");
    {
        QFile touched(filePath);
        QVERIFY(touched.open(QIODevice::ReadWrite));
        QVERIFY(touched.setFileTime(originalMtime, QFileDevice::FileModificationTime));
    }

    // 外部删掉的文件，下一次遍历要把它的索引条目摘掉
    QVERIFY(QFile::remove(root.path() + QStringLiteral("/readme.md")));

    QHash<QString, QString> second;
    {
        SyncServer server(m_serverIdentity, secret);
        QVERIFY2(server.listen(0, root.path()), qPrintable(server.lastError()));
        second = listAll(server, &ok);
    }
    QVERIFY(ok);
    QCOMPARE(second.value(QStringLiteral("docs/a.txt")), first.value(QStringLiteral("docs/a.txt")));
    QVERIFY(!second.contains(QStringLiteral("readme.md")));

    QFile indexFile(indexPath);
    QVERIFY(indexFile.open(QIODevice::ReadOnly));
    const QJsonObject files = QJsonDocument::fromJson(indexFile.readAll()).object()
                                  .value(QStringLiteral("files")).toObject();
    QVERIFY(!files.contains(QStringLiteral("readme.md")));
}

QTEST_MAIN(TestNetServer)
#include "test_net_server.moc"
