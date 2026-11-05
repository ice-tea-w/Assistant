#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslKey>
#include <QTemporaryDir>
#include <QtTest>

#include "core/net/Identity.h"

class TestNetIdentity : public QObject
{
    Q_OBJECT

private slots:
    void generatesValidSelfSignedIdentity();
    void reloadKeepsFingerprint();
    void writesPemFiles();
    void fingerprintIsSha256Hex();
    void reportsEmptyDirectoryPath();
};

void TestNetIdentity::generatesValidSelfSignedIdentity()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    const Sync::Net::Identity identity =
        Sync::Net::loadOrCreateIdentity(temp.filePath(QStringLiteral("identity")),
                                       QStringLiteral("Assistant Test Device"));

    QVERIFY2(identity.error.isEmpty(), qPrintable(identity.error));
    QVERIFY(identity.isValid());
    QVERIFY(identity.certificate.isSelfSigned());
    QVERIFY(identity.certificate.expiryDate() > QDateTime::currentDateTime());
}

void TestNetIdentity::reloadKeepsFingerprint()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString dir = temp.filePath(QStringLiteral("identity"));

    const Sync::Net::Identity first =
        Sync::Net::loadOrCreateIdentity(dir, QStringLiteral("Assistant Test Device"));
    QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
    QVERIFY(!first.fingerprint.isEmpty());

    const Sync::Net::Identity second =
        Sync::Net::loadOrCreateIdentity(dir, QStringLiteral("Assistant Test Device"));
    QVERIFY2(second.error.isEmpty(), qPrintable(second.error));
    QCOMPARE(second.fingerprint, first.fingerprint);
}

void TestNetIdentity::writesPemFiles()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const Sync::Net::Identity identity =
        Sync::Net::loadOrCreateIdentity(temp.filePath(QStringLiteral("identity")),
                                       QStringLiteral("Assistant Test Device"));
    QVERIFY2(identity.error.isEmpty(), qPrintable(identity.error));

    QFile certificateFile(identity.certificatePath);
    QVERIFY(certificateFile.open(QIODevice::ReadOnly));
    const QByteArray certificatePem = certificateFile.readAll();
    QVERIFY(certificatePem.startsWith("-----BEGIN CERTIFICATE-----"));

    QFile keyFile(identity.privateKeyPath);
    QVERIFY(keyFile.open(QIODevice::ReadOnly));
    const QByteArray keyPem = keyFile.readAll();
    QVERIFY(keyPem.startsWith("-----BEGIN PRIVATE KEY-----"));
}

void TestNetIdentity::fingerprintIsSha256Hex()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const Sync::Net::Identity identity =
        Sync::Net::loadOrCreateIdentity(temp.filePath(QStringLiteral("identity")),
                                       QStringLiteral("Assistant Test Device"));
    QVERIFY2(identity.error.isEmpty(), qPrintable(identity.error));

    static const QRegularExpression hex64(QStringLiteral("^[0-9a-f]{64}$"));
    QVERIFY2(hex64.match(identity.fingerprint).hasMatch(), qPrintable(identity.fingerprint));
    QCOMPARE(identity.fingerprint,
             QString::fromLatin1(identity.certificate.digest(QCryptographicHash::Sha256).toHex()));
}

void TestNetIdentity::reportsEmptyDirectoryPath()
{
    const Sync::Net::Identity identity = Sync::Net::loadOrCreateIdentity(QString(), QStringLiteral("x"));
    QVERIFY(!identity.isValid());
    QVERIFY(!identity.error.isEmpty());
}

QTEST_MAIN(TestNetIdentity)
#include "test_net_identity.moc"
