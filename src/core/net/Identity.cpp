#include "core/net/Identity.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QtEndian>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

namespace Sync::Net {
namespace {

// ---- 最小 DER 编码 ----

QByteArray tlv(quint8 tag, const QByteArray &body)
{
    QByteArray out;
    out.append(char(tag));
    const int size = body.size();
    if (size < 0x80) {
        out.append(char(size));
    } else {
        QByteArray length;
        int value = size;
        while (value > 0) {
            length.prepend(char(value & 0xFF));
            value >>= 8;
        }
        out.append(char(0x80 | length.size()));
        out.append(length);
    }
    out.append(body);
    return out;
}

QByteArray seq(const QByteArray &body) { return tlv(0x30, body); }

QByteArray integerFromBytes(QByteArray bytes)
{
    while (bytes.size() > 1 && bytes.at(0) == 0 && (quint8(bytes.at(1)) & 0x80) == 0)
        bytes.remove(0, 1);
    if (bytes.isEmpty() || (quint8(bytes.at(0)) & 0x80))
        bytes.prepend('\0');
    return tlv(0x02, bytes);
}

QByteArray utf8String(const QString &text) { return tlv(0x0C, text.toUtf8()); }

QByteArray utcTime(const QDateTime &time)
{
    return tlv(0x17, time.toUTC().toString(QStringLiteral("yyMMddHHmmss'Z'")).toLatin1());
}

/// payload 前补 0 表示“无未使用位”。
QByteArray bitString(const QByteArray &payload) { return tlv(0x03, QByteArray(1, '\0') + payload); }

const QByteArray kOidEcPublicKey = QByteArray::fromHex("06072A8648CE3D0201");   // 1.2.840.10045.2.1
const QByteArray kOidPrime256v1 = QByteArray::fromHex("06082A8648CE3D030107");  // 1.2.840.10045.3.1.7
const QByteArray kOidEcdsaSha256 = QByteArray::fromHex("06082A8648CE3D040302"); // 1.2.840.10045.4.3.2
const QByteArray kOidCommonName = QByteArray::fromHex("0603550403");            // 2.5.4.3

QByteArray signatureAlgorithm() { return seq(kOidEcdsaSha256); }
QByteArray ecAlgorithmIdentifier() { return seq(kOidEcPublicKey + kOidPrime256v1); }

QByteArray distinguishedName(const QString &commonName)
{
    return seq(tlv(0x31, seq(kOidCommonName + utf8String(commonName))));
}

QByteArray subjectPublicKeyInfo(const QByteArray &x, const QByteArray &y)
{
    QByteArray point(1, '\x04');
    point.append(x);
    point.append(y);
    return seq(ecAlgorithmIdentifier() + bitString(point));
}

QByteArray tbsCertificate(const QString &commonName, const QByteArray &x, const QByteArray &y,
                          const QByteArray &serial)
{
    QByteArray body;
    body.append(tlv(0xA0, integerFromBytes(QByteArray(1, '\x02')))); // version v3
    body.append(integerFromBytes(serial));
    body.append(signatureAlgorithm());
    const QByteArray name = distinguishedName(commonName);
    body.append(name); // issuer
    const QDateTime now = QDateTime::currentDateTimeUtc();
    body.append(seq(utcTime(now.addDays(-1)) + utcTime(now.addYears(10))));
    body.append(name); // subject
    body.append(subjectPublicKeyInfo(x, y));
    return seq(body);
}

QByteArray certificateFrom(const QByteArray &tbs, const QByteArray &signature)
{
    const int half = signature.size() / 2;
    const QByteArray derSignature =
        seq(integerFromBytes(signature.left(half)) + integerFromBytes(signature.mid(half)));
    return seq(tbs + signatureAlgorithm() + bitString(derSignature));
}

/// PKCS#8 PrivateKeyInfo { ECPrivateKey }。Qt 的 DER 私钥入口按 PKCS#8 解析。
QByteArray pkcs8From(const QByteArray &privateScalar, const QByteArray &x, const QByteArray &y)
{
    QByteArray ecPrivateKey;
    ecPrivateKey.append(integerFromBytes(QByteArray(1, '\x01')));
    ecPrivateKey.append(tlv(0x04, privateScalar));
    ecPrivateKey.append(tlv(0xA0, kOidPrime256v1));
    QByteArray point(1, '\x04');
    point.append(x);
    point.append(y);
    ecPrivateKey.append(tlv(0xA1, bitString(point)));

    QByteArray body;
    body.append(integerFromBytes(QByteArray(1, '\0')));
    body.append(ecAlgorithmIdentifier());
    body.append(tlv(0x04, seq(ecPrivateKey)));
    return seq(body);
}

// ---- Windows CNG ----

struct KeyHandle
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;

    ~KeyHandle()
    {
        if (key)
            BCryptDestroyKey(key);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

QString statusText(NTSTATUS status)
{
    return QStringLiteral("0x%1").arg(QString::number(static_cast<qulonglong>(status), 16));
}

bool exportBlob(BCRYPT_KEY_HANDLE key, LPCWSTR type, QByteArray *out, QString *error)
{
    ULONG size = 0;
    NTSTATUS status = BCryptExportKey(key, nullptr, type, nullptr, 0, &size, 0);
    if (!BCRYPT_SUCCESS(status)) {
        *error = QStringLiteral("导出密钥失败：%1").arg(statusText(status));
        return false;
    }
    QByteArray buffer(static_cast<int>(size), '\0');
    status = BCryptExportKey(key, nullptr, type, reinterpret_cast<PUCHAR>(buffer.data()), size, &size, 0);
    if (!BCRYPT_SUCCESS(status)) {
        *error = QStringLiteral("导出密钥失败：%1").arg(statusText(status));
        return false;
    }
    buffer.resize(static_cast<int>(size));
    *out = buffer;
    return true;
}

struct EcBlob
{
    QByteArray x;
    QByteArray y;
    QByteArray d;
};

bool parseBlob(const QByteArray &blob, EcBlob *result, QString *error)
{
    if (blob.size() < int(sizeof(BCRYPT_ECCKEY_BLOB))) {
        *error = QStringLiteral("密钥数据过短");
        return false;
    }
    const BCRYPT_ECCKEY_BLOB header =
        *reinterpret_cast<const BCRYPT_ECCKEY_BLOB *>(blob.constData());
    const int keyBytes = static_cast<int>(header.cbKey);
    if (keyBytes <= 0 || blob.size() < int(sizeof(BCRYPT_ECCKEY_BLOB)) + 2 * keyBytes) {
        *error = QStringLiteral("密钥数据长度不匹配");
        return false;
    }
    const int offset = int(sizeof(BCRYPT_ECCKEY_BLOB));
    result->x = blob.mid(offset, keyBytes);
    result->y = blob.mid(offset + keyBytes, keyBytes);
    if (blob.size() >= offset + 3 * keyBytes)
        result->d = blob.mid(offset + 2 * keyBytes, keyBytes);
    return true;
}

QByteArray randomSerial()
{
    quint64 value = (quint64(QRandomGenerator::system()->generate()) << 32)
                    | quint64(QRandomGenerator::system()->generate());
    QByteArray bytes(8, '\0');
    qToBigEndian<quint64>(value, reinterpret_cast<uchar *>(bytes.data()));
    bytes[0] = char(quint8(bytes.at(0)) & 0x7F);
    if (bytes.at(0) == 0)
        bytes[0] = '\x01';
    return bytes;
}

bool generateIdentity(const QString &commonName, QByteArray *certificateDer,
                      QByteArray *privateKeyDer, QString *error)
{
    KeyHandle handle;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&handle.algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                                  MS_PRIMITIVE_PROVIDER, 0);
    if (!BCRYPT_SUCCESS(status)) {
        *error = QStringLiteral("打开 ECDSA 算法失败：%1").arg(statusText(status));
        return false;
    }
    status = BCryptGenerateKeyPair(handle.algorithm, &handle.key, 256, 0);
    if (!BCRYPT_SUCCESS(status)) {
        *error = QStringLiteral("生成密钥对失败：%1").arg(statusText(status));
        return false;
    }
    status = BCryptFinalizeKeyPair(handle.key, 0);
    if (!BCRYPT_SUCCESS(status)) {
        *error = QStringLiteral("完成密钥对失败：%1").arg(statusText(status));
        return false;
    }

    QByteArray publicBlob;
    QByteArray privateBlob;
    if (!exportBlob(handle.key, BCRYPT_ECCPUBLIC_BLOB, &publicBlob, error))
        return false;
    if (!exportBlob(handle.key, BCRYPT_ECCPRIVATE_BLOB, &privateBlob, error))
        return false;

    EcBlob pub;
    EcBlob priv;
    if (!parseBlob(publicBlob, &pub, error))
        return false;
    if (!parseBlob(privateBlob, &priv, error))
        return false;
    if (priv.d.isEmpty()) {
        *error = QStringLiteral("导出的私钥不含私钥标量");
        return false;
    }

    const QByteArray tbs = tbsCertificate(commonName, pub.x, pub.y, randomSerial());
    const QByteArray digest = QCryptographicHash::hash(tbs, QCryptographicHash::Sha256);

    ULONG signatureSize = 0;
    status = BCryptSignHash(handle.key, nullptr, reinterpret_cast<PUCHAR>(const_cast<char *>(digest.constData())),
                            static_cast<ULONG>(digest.size()), nullptr, 0, &signatureSize, 0);
    if (!BCRYPT_SUCCESS(status)) {
        *error = QStringLiteral("签名失败：%1").arg(statusText(status));
        return false;
    }
    QByteArray signature(static_cast<int>(signatureSize), '\0');
    status = BCryptSignHash(handle.key, nullptr, reinterpret_cast<PUCHAR>(const_cast<char *>(digest.constData())),
                            static_cast<ULONG>(digest.size()), reinterpret_cast<PUCHAR>(signature.data()),
                            signatureSize, &signatureSize, 0);
    if (!BCRYPT_SUCCESS(status)) {
        *error = QStringLiteral("签名失败：%1").arg(statusText(status));
        return false;
    }
    signature.resize(static_cast<int>(signatureSize));

    *certificateDer = certificateFrom(tbs, signature);
    *privateKeyDer = pkcs8From(priv.d, pub.x, pub.y);
    return true;
}

} // namespace

QString fingerprintOf(const QSslCertificate &certificate)
{
    if (certificate.isNull())
        return QString();
    return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex());
}

Identity loadOrCreateIdentity(const QString &dirPath, const QString &commonName)
{
    Identity identity;
    if (dirPath.isEmpty()) {
        identity.error = QStringLiteral("身份目录路径为空");
        return identity;
    }
    QDir directory(dirPath);
    if (!directory.exists() && !QDir().mkpath(dirPath)) {
        identity.error = QStringLiteral("无法创建身份目录：%1").arg(dirPath);
        return identity;
    }

    identity.certificatePath = directory.filePath(QStringLiteral("device-cert.pem"));
    identity.privateKeyPath = directory.filePath(QStringLiteral("device-key.pem"));

    QFile certificateFile(identity.certificatePath);
    QFile keyFile(identity.privateKeyPath);
    if (certificateFile.exists() && keyFile.exists()) {
        if (!certificateFile.open(QIODevice::ReadOnly)) {
            identity.error = QStringLiteral("无法读取身份证书：%1").arg(certificateFile.errorString());
            return identity;
        }
        if (!keyFile.open(QIODevice::ReadOnly)) {
            identity.error = QStringLiteral("无法读取身份私钥：%1").arg(keyFile.errorString());
            return identity;
        }
        identity.certificate = QSslCertificate(certificateFile.readAll(), QSsl::Pem);
        identity.privateKey = QSslKey(keyFile.readAll(), QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    } else {
        QByteArray certificateDer;
        QByteArray privateKeyDer;
        if (!generateIdentity(commonName, &certificateDer, &privateKeyDer, &identity.error))
            return identity;

        identity.certificate = QSslCertificate(certificateDer, QSsl::Der);
        identity.privateKey = QSslKey(privateKeyDer, QSsl::Ec, QSsl::Der, QSsl::PrivateKey);
        if (identity.certificate.isNull()) {
            identity.error = QStringLiteral("生成的证书无法解析");
            return identity;
        }
        if (identity.privateKey.isNull()) {
            identity.error = QStringLiteral("当前 TLS 后端无法载入内存中的 EC 私钥");
            return identity;
        }

        if (!certificateFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || certificateFile.write(identity.certificate.toPem()) < 0) {
            identity.error = QStringLiteral("无法写入身份证书：%1").arg(certificateFile.errorString());
            return identity;
        }
        if (!keyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || keyFile.write(identity.privateKey.toPem()) < 0) {
            identity.error = QStringLiteral("无法写入身份私钥：%1").arg(keyFile.errorString());
            return identity;
        }
    }

    if (identity.certificate.isNull()) {
        identity.error = QStringLiteral("身份证书解析失败");
        return identity;
    }
    if (identity.privateKey.isNull()) {
        identity.error = QStringLiteral("当前 TLS 后端无法载入 EC 私钥");
        return identity;
    }
    identity.fingerprint = fingerprintOf(identity.certificate);
    return identity;
}

} // namespace Sync::Net
