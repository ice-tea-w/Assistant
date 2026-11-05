#include <QHostAddress>
#include <QtTest>

#include "core/net/PeerAddress.h"
#include "core/net/SyncFrame.h"

using namespace Sync::Net;

namespace {

QByteArray manualHeader(const char *magic, quint8 version, quint8 type, quint16 flags,
                        quint32 reqId, quint32 payloadLen)
{
    QByteArray bytes;
    bytes.append(magic, 4);
    bytes.append(char(version));
    bytes.append(char(type));
    const quint16 f = qToLittleEndian(flags);
    bytes.append(reinterpret_cast<const char *>(&f), 2);
    const quint32 id = qToLittleEndian(reqId);
    bytes.append(reinterpret_cast<const char *>(&id), 4);
    const quint32 len = qToLittleEndian(payloadLen);
    bytes.append(reinterpret_cast<const char *>(&len), 4);
    return bytes;
}

} // namespace

class TestNetProtocol : public QObject {
    Q_OBJECT

private slots:
    void frameRoundTrip();
    void byteByByteReassembly();
    void twoFramesInOneChunk();
    void truncatedPayloadWaits();
    void rejectsBadMagic();
    void rejectsBadVersion();
    void rejectsOversizedLength();
    void encodesAtSizeLimit();
    void rejectsOversizedEncode();
    void parseAddress_data();
    void parseAddress();
};

void TestNetProtocol::frameRoundTrip()
{
    Frame in;
    in.type = FrameType::List;
    in.flags = FlagJson;
    in.reqId = 42;
    in.payload = QByteArrayLiteral("{}");

    const QByteArray bytes = encodeFrame(in);
    QVERIFY(!bytes.isEmpty());
    QCOMPARE(bytes.size(), kHeaderSize + in.payload.size());
    QCOMPARE(bytes.left(4), QByteArrayLiteral("ASYN"));

    FrameDecoder decoder;
    decoder.append(bytes);

    Frame out;
    QString error;
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Frame);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(quint8(out.type), quint8(FrameType::List));
    QCOMPARE(out.flags, quint16(FlagJson));
    QCOMPARE(out.reqId, quint32(42));
    QCOMPARE(out.payload, QByteArrayLiteral("{}"));
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::NeedMore);
    QCOMPARE(decoder.buffered(), 0);
}

void TestNetProtocol::byteByByteReassembly()
{
    Frame a;
    a.type = FrameType::Put;
    a.flags = FlagJson;
    a.reqId = 1;
    a.payload = QByteArrayLiteral("{\"relPath\":\"a.txt\"}");

    Frame b;
    b.type = FrameType::Chunk;
    b.flags = FlagBlob;
    b.reqId = 2;
    b.payload = QByteArray(1000, 'z');

    const QByteArray bytes = encodeFrame(a) + encodeFrame(b);

    FrameDecoder decoder;
    QList<Frame> frames;
    QString error;
    for (int i = 0; i < bytes.size(); ++i) {
        decoder.append(bytes.mid(i, 1));
        while (true) {
            Frame out;
            const auto status = decoder.next(&out, &error);
            if (status == FrameDecoder::Status::Frame) {
                frames.append(out);
            } else if (status == FrameDecoder::Status::NeedMore) {
                break;
            } else {
                QFAIL(qPrintable(error));
            }
        }
    }

    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).payload, a.payload);
    QCOMPARE(quint8(frames.at(0).type), quint8(FrameType::Put));
    QCOMPARE(frames.at(1).payload, b.payload);
    QCOMPARE(frames.at(1).reqId, quint32(2));
}

void TestNetProtocol::twoFramesInOneChunk()
{
    Frame a;
    a.type = FrameType::Ping;
    a.payload = QByteArrayLiteral("{}");
    Frame b;
    b.type = FrameType::Pong;
    b.payload = QByteArrayLiteral("{}");

    FrameDecoder decoder;
    decoder.append(encodeFrame(a) + encodeFrame(b));

    Frame out;
    QString error;
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Frame);
    QCOMPARE(quint8(out.type), quint8(FrameType::Ping));
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Frame);
    QCOMPARE(quint8(out.type), quint8(FrameType::Pong));
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::NeedMore);
}

void TestNetProtocol::truncatedPayloadWaits()
{
    const QByteArray header = manualHeader("ASYN", kVersion, quint8(FrameType::Chunk),
                                           0x0002, 7, 4);
    QCOMPARE(header.size(), kHeaderSize);

    FrameDecoder decoder;
    decoder.append(header + QByteArray("ab", 2));

    Frame out;
    QString error;
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::NeedMore);
    QVERIFY(!decoder.failed());
    QCOMPARE(decoder.buffered(), kHeaderSize + 2);

    decoder.append(QByteArray("cd", 2));
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Frame);
    QCOMPARE(out.payload, QByteArrayLiteral("abcd"));
    QCOMPARE(out.reqId, quint32(7));
}

void TestNetProtocol::rejectsBadMagic()
{
    FrameDecoder decoder;
    decoder.append(manualHeader("XSYN", kVersion, quint8(FrameType::Ping), 0, 0, 0));

    Frame out;
    QString error;
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Error);
    QVERIFY(!error.isEmpty());
    QVERIFY(decoder.failed());

    error.clear();
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Error);
    QVERIFY(!error.isEmpty());
}

void TestNetProtocol::rejectsBadVersion()
{
    FrameDecoder decoder;
    decoder.append(manualHeader("ASYN", 0x03, quint8(FrameType::Ping), 0, 0, 0));

    Frame out;
    QString error;
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Error);
    QVERIFY(!error.isEmpty());
}

void TestNetProtocol::rejectsOversizedLength()
{
    FrameDecoder decoder;
    decoder.append(manualHeader("ASYN", kVersion, quint8(FrameType::Chunk), 0, 0,
                                kMaxPayload + 1));

    Frame out;
    QString error;
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Error);
    QVERIFY(!error.isEmpty());
}

void TestNetProtocol::encodesAtSizeLimit()
{
    Frame in;
    in.type = FrameType::PutChunk;
    in.flags = FlagBlob;
    in.payload = QByteArray(int(kMaxPayload), 'x');

    const QByteArray bytes = encodeFrame(in);
    QVERIFY(!bytes.isEmpty());

    FrameDecoder decoder;
    decoder.append(bytes);
    Frame out;
    QString error;
    QCOMPARE(decoder.next(&out, &error), FrameDecoder::Status::Frame);
    QCOMPARE(out.payload.size(), int(kMaxPayload));
}

void TestNetProtocol::rejectsOversizedEncode()
{
    Frame in;
    in.type = FrameType::PutChunk;
    in.payload = QByteArray(int(kMaxPayload) + 1, 'x');
    QVERIFY(encodeFrame(in).isEmpty());
}

void TestNetProtocol::parseAddress_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<quint16>("defaultPort");
    QTest::addColumn<QString>("expectedHost");
    QTest::addColumn<int>("expectedPort");
    QTest::addColumn<bool>("ok");

    QTest::newRow("ipv4 带端口") << "1.2.3.4:45871" << quint16(45871) << "1.2.3.4" << 45871 << true;
    QTest::newRow("ipv6 带端口") << "[2408:1234::1]:5000" << quint16(45871) << "2408:1234::1" << 5000
                               << true;
    QTest::newRow("ipv6 默认端口") << "[2408:1234::1]" << quint16(45871) << "2408:1234::1" << 45871
                                 << true;
    QTest::newRow("域名带端口") << "nas.local:9000" << quint16(45871) << "nas.local" << 9000 << true;
    QTest::newRow("域名默认端口") << "nas.local" << quint16(45871) << "nas.local" << 45871 << true;
    QTest::newRow("两端空白") << "  1.2.3.4:1  " << quint16(45871) << "1.2.3.4" << 1 << true;
    QTest::newRow("裸 ipv6 拒绝") << "2001:db8::1" << quint16(45871) << "" << 0 << false;
    QTest::newRow("端口 0 拒绝") << "[::1]:0" << quint16(45871) << "" << 0 << false;
    QTest::newRow("端口超限拒绝") << "[::1]:99999" << quint16(45871) << "" << 0 << false;
    QTest::newRow("端口非数字拒绝") << "[::1]:abc" << quint16(45871) << "" << 0 << false;
    QTest::newRow("空串拒绝") << "" << quint16(45871) << "" << 0 << false;
    QTest::newRow("冒号后为空拒绝") << "host:" << quint16(45871) << "" << 0 << false;
    QTest::newRow("主机含空格拒绝") << "a b:1" << quint16(45871) << "" << 0 << false;
    QTest::newRow("方括号未闭合拒绝") << "[::1" << quint16(45871) << "" << 0 << false;
}

void TestNetProtocol::parseAddress()
{
    QFETCH(QString, input);
    QFETCH(quint16, defaultPort);
    QFETCH(QString, expectedHost);
    QFETCH(int, expectedPort);
    QFETCH(bool, ok);

    const PeerAddress result = parsePeerAddress(input, defaultPort);
    QCOMPARE(result.ok, ok);
    if (ok) {
        QCOMPARE(result.host, expectedHost);
        QCOMPARE(int(result.port), expectedPort);
        QVERIFY(result.error.isEmpty());
    } else {
        QVERIFY(!result.error.isEmpty());
    }
}

QTEST_MAIN(TestNetProtocol)
#include "test_net_protocol.moc"
