#include "core/net/DeviceAuth.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QtEndian>

namespace Sync::Net {
namespace {

const char kDomainTag[] = "ASYN-AUTH-V1";

void appendField(QByteArray *message, const QByteArray &value)
{
    uchar length[4];
    qToLittleEndian<quint32>(quint32(value.size()), length);
    message->append(reinterpret_cast<const char *>(length), 4);
    message->append(value);
}

} // namespace

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    constexpr int blockSize = 64;
    QByteArray normalized = key;
    if (normalized.size() > blockSize)
        normalized = QCryptographicHash::hash(normalized, QCryptographicHash::Sha256);
    normalized.resize(blockSize, '\0');

    QByteArray innerPad(blockSize, '\0');
    QByteArray outerPad(blockSize, '\0');
    for (int index = 0; index < blockSize; ++index) {
        innerPad[index] = char(normalized.at(index) ^ 0x36);
        outerPad[index] = char(normalized.at(index) ^ 0x5c);
    }

    const QByteArray innerHash =
        QCryptographicHash::hash(innerPad + data, QCryptographicHash::Sha256);
    return QCryptographicHash::hash(outerPad + innerHash, QCryptographicHash::Sha256);
}

QByteArray challengeResponse(const QByteArray &deviceSecret, const QByteArray &nonce,
                             const QString &deviceId, const QString &serverFingerprint)
{
    QByteArray message(kDomainTag);
    appendField(&message, deviceId.toUtf8());
    appendField(&message, nonce);
    appendField(&message, serverFingerprint.toUtf8());
    return hmacSha256(deviceSecret, message);
}

bool verifyChallengeResponse(const QByteArray &expected, const QByteArray &actual)
{
    if (expected.size() != actual.size())
        return false;
    uchar difference = 0;
    for (int index = 0; index < expected.size(); ++index)
        difference |= uchar(expected.at(index) ^ actual.at(index));
    return difference == 0;
}

QByteArray randomBytes(int size)
{
    QByteArray bytes(size, '\0');
    for (int offset = 0; offset < size; offset += 8) {
        const quint64 value = QRandomGenerator::system()->generate64();
        const int chunk = qMin(8, size - offset);
        bytes.replace(offset, chunk, QByteArray(reinterpret_cast<const char *>(&value), chunk));
    }
    return bytes;
}

} // namespace Sync::Net
