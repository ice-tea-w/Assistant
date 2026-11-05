#include <QSet>
#include <QtTest>

#include "core/net/DeviceAuth.h"

class TestNetAuth : public QObject
{
    Q_OBJECT

private slots:
    void hmacMatchesRfc4231();
    void responseBindsEveryField();
    void verifyAcceptsOnlyExactMatch();
    void randomBytesAreSized();
};

void TestNetAuth::hmacMatchesRfc4231()
{
    // RFC 4231 用例 1 与 2，防止自写 HMAC 出现填充错误。
    QCOMPARE(Sync::Net::hmacSha256(QByteArray(20, '\x0b'), QByteArray("Hi There")).toHex(),
             QByteArray("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    QCOMPARE(Sync::Net::hmacSha256(QByteArray("Jefe"), QByteArray("what do ya want for nothing?")).toHex(),
             QByteArray("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

void TestNetAuth::responseBindsEveryField()
{
    const QByteArray secret = QByteArray(Sync::Net::kDeviceSecretSize, '\x11');
    const QByteArray nonce = QByteArray(Sync::Net::kChallengeSize, '\x22');
    const QString deviceId = QStringLiteral("device-a");
    const QString fingerprint = QStringLiteral("ab12");

    const QByteArray baseline = Sync::Net::challengeResponse(secret, nonce, deviceId, fingerprint);
    QCOMPARE(baseline.size(), 32);

    QVERIFY(baseline != Sync::Net::challengeResponse(QByteArray(32, '\x12'), nonce, deviceId, fingerprint));
    QVERIFY(baseline != Sync::Net::challengeResponse(secret, QByteArray(32, '\x23'), deviceId, fingerprint));
    QVERIFY(baseline != Sync::Net::challengeResponse(secret, nonce, QStringLiteral("device-b"), fingerprint));
    QVERIFY(baseline != Sync::Net::challengeResponse(secret, nonce, deviceId, QStringLiteral("ab13")));
    // 拼接歧义防护："device" + "a" 与 "devicea" + "" 必须不同。
    QVERIFY(Sync::Net::challengeResponse(secret, nonce, QStringLiteral("device"), QStringLiteral("a"))
            != Sync::Net::challengeResponse(secret, nonce, QStringLiteral("devicea"), QString()));
}

void TestNetAuth::verifyAcceptsOnlyExactMatch()
{
    const QByteArray expected = QByteArray(32, '\x33');
    QVERIFY(Sync::Net::verifyChallengeResponse(expected, expected));

    QByteArray tampered = expected;
    tampered[31] = char(tampered.at(31) ^ 0x01);
    QVERIFY(!Sync::Net::verifyChallengeResponse(expected, tampered));
    QVERIFY(!Sync::Net::verifyChallengeResponse(expected, expected.left(31)));
    QVERIFY(!Sync::Net::verifyChallengeResponse(expected, QByteArray()));
}

void TestNetAuth::randomBytesAreSized()
{
    const QByteArray first = Sync::Net::randomBytes(Sync::Net::kDeviceSecretSize);
    const QByteArray second = Sync::Net::randomBytes(Sync::Net::kDeviceSecretSize);
    QCOMPARE(first.size(), 32);
    QCOMPARE(second.size(), 32);
    QVERIFY(first != second);
    QCOMPARE(Sync::Net::randomBytes(5).size(), 5);
    QCOMPARE(Sync::Net::randomBytes(0).size(), 0);
    QVERIFY(Sync::Net::randomBytes(Sync::Net::kChallengeSize)
            != Sync::Net::randomBytes(Sync::Net::kChallengeSize));
}

QTEST_MAIN(TestNetAuth)
#include "test_net_auth.moc"
