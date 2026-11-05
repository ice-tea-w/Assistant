#include "core/net/Identity.h"
#include "core/net/NetTransport.h"
#include "core/net/SyncServer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>

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

/// 服务端必须活在**另一个有事件循环的线程**里：
/// NetTransport 是阻塞式的（waitFor*），同线程会互相等死。设计 §5.4。
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

class TestNetTransport : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void probeRejectsFingerprintMismatch();
    void probeRejectsUnknownDevice();
    void listRemoteReturnsFiles();
    void fetchWritesFile();
    void pushUploadsFile();
    void removeRemoteMovesToTrash();

private:
    Sync::Net::NetPeer peer() const;

    QTemporaryDir         m_root;
    QString               m_sharedRoot;
    QString               m_deviceId   = QStringLiteral("client-1");
    QByteArray            m_secret     = QByteArray::fromHex("00112233445566778899aabbccddeeff"
                                                             "00112233445566778899aabbccddeeff");
    Sync::Net::Identity   m_serverIdentity;
    ServerRunner         *m_runner = nullptr;
};

Sync::Net::NetPeer TestNetTransport::peer() const
{
    Sync::Net::NetPeer out;
    out.host                = QStringLiteral("127.0.0.1");
    out.port                = m_runner->port.load();
    out.expectedFingerprint = m_serverIdentity.fingerprint;
    out.deviceId            = m_deviceId;
    out.deviceSecret        = m_secret;
    out.timeoutMs           = 5000;
    return out;
}

void TestNetTransport::initTestCase()
{
    QVERIFY(m_root.isValid());

    m_sharedRoot = m_root.filePath(QStringLiteral("shared"));
    QVERIFY(QDir().mkpath(m_sharedRoot));
    writeFile(m_sharedRoot + QStringLiteral("/docs/a.txt"), "aaa");
    writeFile(m_sharedRoot + QStringLiteral("/docs/b.txt"), "bbbb");

    m_serverIdentity = Sync::Net::loadOrCreateIdentity(m_root.filePath(QStringLiteral("identity")),
                                                       QStringLiteral("Assistant Test Server"));
    QVERIFY2(m_serverIdentity.isValid(), qPrintable(m_serverIdentity.error));

    m_runner = new ServerRunner;
    m_runner->identity   = m_serverIdentity;
    m_runner->sharedRoot = m_sharedRoot;
    m_runner->deviceId   = m_deviceId;
    m_runner->secret     = m_secret;
    m_runner->start();

    QTRY_VERIFY_WITH_TIMEOUT(m_runner->port.load() != 0 || !m_runner->failure.isEmpty(), 5000);
    QVERIFY2(m_runner->failure.isEmpty(), qPrintable(m_runner->failure));
}

void TestNetTransport::cleanupTestCase()
{
    if (m_runner) {
        m_runner->quit();
        m_runner->wait(5000);
        delete m_runner;
        m_runner = nullptr;
    }
}

void TestNetTransport::probeRejectsFingerprintMismatch()
{
    Sync::Net::NetPeer bad = peer();
    bad.expectedFingerprint = QString(64, QLatin1Char('a'));

    Sync::Net::NetTransport transport(bad);
    QVERIFY(!transport.probe());
    QVERIFY2(transport.lastError().contains(QStringLiteral("指纹不匹配")),
             qPrintable(transport.lastError()));
}

void TestNetTransport::probeRejectsUnknownDevice()
{
    Sync::Net::NetPeer bad = peer();
    bad.deviceId = QStringLiteral("stranger");

    Sync::Net::NetTransport transport(bad);
    QVERIFY(!transport.probe());
    QVERIFY(!transport.lastError().isEmpty());
}

void TestNetTransport::listRemoteReturnsFiles()
{
    Sync::Net::NetTransport transport(peer());
    QVERIFY2(transport.probe(), qPrintable(transport.lastError()));

    const QList<Sync::RemoteFileInfo> files = transport.listRemote();
    QCOMPARE(files.size(), 2);

    QStringList paths;
    for (const Sync::RemoteFileInfo &file : files)
        paths.append(file.relPath);
    paths.sort();
    QCOMPARE(paths, QStringList({QStringLiteral("docs/a.txt"), QStringLiteral("docs/b.txt")}));

    QCOMPARE(transport.remoteAliases(), QStringList({QStringLiteral("docs")}));
}

void TestNetTransport::fetchWritesFile()
{
    Sync::Net::NetTransport transport(peer());
    const QString dest = m_root.filePath(QStringLiteral("downloads/a.txt"));
    QVERIFY(QDir().mkpath(QFileInfo(dest).absolutePath()));

    QVERIFY2(transport.fetch(QStringLiteral("docs/a.txt"), dest), qPrintable(transport.lastError()));
    QCOMPARE(readFile(dest), QByteArray("aaa"));
    QVERIFY(!QFile::exists(dest + QStringLiteral(".assistant-part")));
}

void TestNetTransport::pushUploadsFile()
{
    const QString local = m_root.filePath(QStringLiteral("upload/local.txt"));
    writeFile(local, "hello over tls");

    Sync::Net::NetTransport transport(peer());
    QVERIFY2(transport.push(local, QStringLiteral("docs/c.txt")), qPrintable(transport.lastError()));

    QCOMPARE(readFile(m_sharedRoot + QStringLiteral("/docs/c.txt")), QByteArray("hello over tls"));
    QVERIFY(!QFile::exists(m_sharedRoot + QStringLiteral("/docs/c.txt.assistant-part")));
}

void TestNetTransport::removeRemoteMovesToTrash()
{
    const QString target = m_sharedRoot + QStringLiteral("/docs/b.txt");
    QVERIFY(QFile::exists(target));

    Sync::Net::NetTransport transport(peer());
    QVERIFY2(transport.removeRemote(QStringLiteral("docs/b.txt")), qPrintable(transport.lastError()));
    QVERIFY(!QFile::exists(target));
}

QTEST_MAIN(TestNetTransport)
#include "test_net_transport.moc"
