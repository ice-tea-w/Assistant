#include "core/net/Identity.h"
#include "core/net/NetTransport.h"
#include "core/net/SyncServer.h"
#include "core/sync/SyncEngine.h"
#include "database/SyncDatabase.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

class ServerRunner : public QThread
{
public:
    Sync::Net::Identity identity;
    QString             sharedRoot;
    QString             deviceId;
    QByteArray          secret;
    std::atomic<quint16> port{0};
    QString             failure;

    void run() override
    {
        Sync::Net::SyncServer server(identity, [this](const QString &id) {
            return id == deviceId ? secret : QByteArray();
        });
        if (!server.listen(0, sharedRoot)) {
            failure = server.lastError();
            return;
        }
        port = server.port();
        exec();
    }
};

} // namespace

/// N2 的最后一块拼图：把 NetTransport 塞进真正的 SyncEngine，
/// 验证"三方差集判断"在新通道上一格不改地跑通。
class TestNetEngine : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void uploadsLocalFiles();
    void secondSyncIsNoop();
    void downloadsRemoteFile();

private:
    QTemporaryDir       m_root;
    QString             m_local;
    QString             m_shared;
    QString             m_deviceId = QStringLiteral("engine-client");
    QByteArray          m_secret   = QByteArray::fromHex("a1a2a3a4a5a6a7a8a9aaabacadaeafb0"
                                                         "a1a2a3a4a5a6a7a8a9aaabacadaeafb0");
    Sync::Net::Identity m_identity;
    ServerRunner       *m_runner = nullptr;
    SyncDatabase       *m_db     = nullptr;
    Sync::SyncEngine   *m_engine = nullptr;
};

void TestNetEngine::initTestCase()
{
    QVERIFY(m_root.isValid());
    m_local  = m_root.filePath(QStringLiteral("local"));
    m_shared = m_root.filePath(QStringLiteral("shared"));
    QVERIFY(QDir().mkpath(m_local));
    QVERIFY(QDir().mkpath(m_shared));

    m_identity = Sync::Net::loadOrCreateIdentity(m_root.filePath(QStringLiteral("identity")),
                                                 QStringLiteral("Assistant Engine Server"));
    QVERIFY2(m_identity.isValid(), qPrintable(m_identity.error));

    m_runner = new ServerRunner;
    m_runner->identity   = m_identity;
    m_runner->sharedRoot = m_shared;
    m_runner->deviceId   = m_deviceId;
    m_runner->secret     = m_secret;
    m_runner->start();
    QTRY_VERIFY_WITH_TIMEOUT(m_runner->port.load() != 0 || !m_runner->failure.isEmpty(), 5000);
    QVERIFY2(m_runner->failure.isEmpty(), qPrintable(m_runner->failure));

    m_db = new SyncDatabase(QStringLiteral("net_engine_test"));
    QVERIFY2(m_db->open(m_root.filePath(QStringLiteral("sync.db"))), "打不开测试用 sync.db");

    Sync::Net::NetPeer peer;
    peer.host                = QStringLiteral("127.0.0.1");
    peer.port                = m_runner->port.load();
    peer.expectedFingerprint = m_identity.fingerprint;
    peer.deviceId            = m_deviceId;
    peer.deviceSecret        = m_secret;

    m_engine = new Sync::SyncEngine;
    m_engine->setDatabase(m_db);
    m_engine->setDeviceName(QStringLiteral("host-a"));
    m_engine->setRoots({{m_local, QStringLiteral("work")}});
    m_engine->setTransports({std::make_shared<Sync::Net::NetTransport>(peer)});

    writeFile(m_local + QStringLiteral("/a.txt"), "aaa");
    writeFile(m_local + QStringLiteral("/sub/b.txt"), "bbbb");
}

void TestNetEngine::cleanupTestCase()
{
    delete m_engine;
    m_engine = nullptr;
    if (m_db) {
        m_db->close();
        delete m_db;
        m_db = nullptr;
    }
    if (m_runner) {
        m_runner->quit();
        m_runner->wait(5000);
        delete m_runner;
        m_runner = nullptr;
    }
}

void TestNetEngine::uploadsLocalFiles()
{
    const Sync::SyncResult result = m_engine->sync();
    QVERIFY2(result.status == Sync::SyncResult::Ok, qPrintable(result.message));
    QCOMPARE(result.uploaded, 2);
    QCOMPARE(result.downloaded, 0);
    QCOMPARE(result.failed, 0);

    QCOMPARE(readFile(m_shared + QStringLiteral("/work/a.txt")), QByteArray("aaa"));
    QCOMPARE(readFile(m_shared + QStringLiteral("/work/sub/b.txt")), QByteArray("bbbb"));
}

void TestNetEngine::secondSyncIsNoop()
{
    const Sync::SyncResult result = m_engine->sync();
    QVERIFY2(result.status == Sync::SyncResult::Ok, qPrintable(result.message));
    QCOMPARE(result.uploaded, 0);
    QCOMPARE(result.downloaded, 0);
    QCOMPARE(result.deleted, 0);
}

void TestNetEngine::downloadsRemoteFile()
{
    writeFile(m_shared + QStringLiteral("/work/remote.txt"), "from remote");

    const Sync::SyncResult result = m_engine->sync();
    QVERIFY2(result.status == Sync::SyncResult::Ok, qPrintable(result.message));
    QCOMPARE(result.downloaded, 1);
    QCOMPARE(readFile(m_local + QStringLiteral("/remote.txt")), QByteArray("from remote"));
}

QTEST_MAIN(TestNetEngine)
#include "test_net_engine.moc"
