#include "core/net/Identity.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <string>

namespace Sync::Net {
namespace {

const char kCertificateFile[] = "device-cert.pem";
const char kPrivateKeyFile[] = "device-key.pem";
const char kDefaultCommonName[] = "Assistant Sync Device";
const int kKeyBits = 2048;
const int kValidityYears = 10;

QByteArray tlv(quint8 tag, const QByteArray &body)
{
    QByteArray out;
    out.append(char(tag));
    const int length = body.size();
    if (length < 0x80) {
        out.append(char(length));
    } else {
        QByteArray lengthBytes;
        for (int remaining = length; remaining > 0; remaining >>= 8)
            lengthBytes.prepend(char(remaining & 0xFF));
        out.append(char(0x80 | lengthBytes.size()));
        out.append(lengthBytes);
    }
    out.append(body);
    return out;
}

QByteArray sequence(const QByteArray &body)
{
    return tlv(0x30, body);
}

/// CAPI 的 RSA 字段是小端，DER INTEGER 要大端。
QByteArray bigEndian(const QByteArray &littleEndianValue)
{
    QByteArray value = littleEndianValue;
    std::reverse(value.begin(), value.end());
    return value;
}

QByteArray integer(const QByteArray &bigEndianValue)
{
    QByteArray value = bigEndianValue;
    while (value.size() > 1 && value.at(0) == '\0')
        value.remove(0, 1);
    if (value.isEmpty() || (quint8(value.at(0)) & 0x80))
        value.prepend('\0');
    return tlv(0x02, value);
}

QByteArray rsaAlgorithmIdentifier()
{
    static const QByteArray oid = QByteArray::fromHex("06092A864886F70D010101");
    static const QByteArray nullParameter = QByteArray::fromHex("0500");
    return sequence(oid + nullParameter);
}

quint32 readU32(const uchar *data)
{
    return quint32(data[0]) | (quint32(data[1]) << 8) | (quint32(data[2]) << 16) | (quint32(data[3]) << 24);
}

/// CAPI 的 PRIVATEKEYBLOB → PKCS#8 DER（Qt 只吃 PKCS#8 或 PKCS#1 的 PEM/DER）。
QByteArray pkcs8FromPrivateBlob(const QByteArray &blob, QString *error)
{
    constexpr int blobHeaderSize = 8;     // bType + bVersion + reserved + aiKeyAlg
    constexpr int rsaPubKeySize = 12;     // magic + bitlen + pubexp
    constexpr quint32 rsaTwoMagic = 0x32415352; // 'RSA2'

    if (blob.size() < blobHeaderSize + rsaPubKeySize) {
        *error = QStringLiteral("私钥数据不完整（PRIVATEKEYBLOB 头部缺失）");
        return {};
    }
    const uchar *data = reinterpret_cast<const uchar *>(blob.constData());
    if (data[0] != PRIVATEKEYBLOB || readU32(data + 8) != rsaTwoMagic) {
        *error = QStringLiteral("私钥不是 CAPI 的 RSA PRIVATEKEYBLOB");
        return {};
    }

    const int bitLength = int(readU32(data + 12));
    const quint32 publicExponent = readU32(data + 16);
    const int modulusSize = bitLength / 8;
    const int primeSize = bitLength / 16;
    const int offset = blobHeaderSize + rsaPubKeySize;
    const int expectedSize = offset + modulusSize * 2 + primeSize * 5;
    if (bitLength <= 0 || blob.size() < expectedSize) {
        *error = QStringLiteral("私钥数据不完整（期望 %1 字节，实得 %2）").arg(expectedSize).arg(blob.size());
        return {};
    }

    int position = offset;
    auto take = [&](int size) {
        const QByteArray value = bigEndian(blob.mid(position, size));
        position += size;
        return value;
    };
    const QByteArray modulus = take(modulusSize);
    const QByteArray prime1 = take(primeSize);
    const QByteArray prime2 = take(primeSize);
    const QByteArray exponent1 = take(primeSize);
    const QByteArray exponent2 = take(primeSize);
    const QByteArray coefficient = take(primeSize);
    const QByteArray privateExponent = take(modulusSize);

    QByteArray exponent;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const uchar byte = uchar(publicExponent >> shift);
        if (!exponent.isEmpty() || byte != 0)
            exponent.append(char(byte));
    }
    if (exponent.isEmpty())
        exponent.append('\0');

    const QByteArray pkcs1 = sequence(integer(QByteArray(1, '\0')) + integer(modulus) + integer(exponent)
                                      + integer(privateExponent) + integer(prime1) + integer(prime2)
                                      + integer(exponent1) + integer(exponent2) + integer(coefficient));
    return sequence(integer(QByteArray(1, '\0')) + rsaAlgorithmIdentifier() + tlv(0x04, pkcs1));
}

/// 自己写 PEM，避免不同 TLS 后端把 PKCS#8 转成 PKCS#1 造成格式漂移。
QByteArray pemEncode(const QByteArray &der, const char *label)
{
    QByteArray pem;
    pem.append("-----BEGIN ");
    pem.append(label);
    pem.append("-----\n");
    const QByteArray base64 = der.toBase64();
    for (int offset = 0; offset < base64.size(); offset += 64) {
        pem.append(base64.mid(offset, 64));
        pem.append('\n');
    }
    pem.append("-----END ");
    pem.append(label);
    pem.append("-----\n");
    return pem;
}

SYSTEMTIME toSystemTime(const QDateTime &dateTime)
{
    SYSTEMTIME value{};
    value.wYear = WORD(dateTime.date().year());
    value.wMonth = WORD(dateTime.date().month());
    value.wDay = WORD(dateTime.date().day());
    value.wHour = WORD(dateTime.time().hour());
    value.wMinute = WORD(dateTime.time().minute());
    value.wSecond = WORD(dateTime.time().second());
    return value;
}

QString windowsError(const QString &what, DWORD code)
{
    return QStringLiteral("%1失败（错误码 %2）").arg(what).arg(code);
}

/// 用 CryptoAPI 生成 RSA-2048 自签证书并导出 PKCS#8 私钥。
/// 临时密钥容器用完即删，不在用户 profile 里留密钥。
bool generateIdentity(const QString &commonName, QByteArray *certificateDer, QByteArray *pkcs8, QString *error)
{
    const QString containerName = QStringLiteral("AssistantSyncIdentity-%1")
                                      .arg(QRandomGenerator::global()->generate64(), 16, 16, QLatin1Char('0'));
    const std::wstring container = containerName.toStdWString();
    const std::wstring subjectName = QStringLiteral("CN=%1").arg(commonName).toStdWString();

    HCRYPTPROV provider = 0;
    HCRYPTKEY key = 0;
    bool containerCreated = false;
    auto cleanup = [&] {
        if (key) {
            CryptDestroyKey(key);
            key = 0;
        }
        if (provider) {
            CryptReleaseContext(provider, 0);
            provider = 0;
        }
        if (containerCreated)
            CryptAcquireContextW(&provider, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                                 CRYPT_DELETEKEYSET);
    };

    if (!CryptAcquireContextW(&provider, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                              CRYPT_NEWKEYSET | CRYPT_SILENT)) {
        *error = windowsError(QStringLiteral("创建密钥容器"), GetLastError());
        return false;
    }
    containerCreated = true;

    if (!CryptGenKey(provider, AT_SIGNATURE, (DWORD(kKeyBits) << 16) | CRYPT_EXPORTABLE, &key)) {
        *error = windowsError(QStringLiteral("生成 %1 位 RSA 密钥").arg(kKeyBits), GetLastError());
        cleanup();
        return false;
    }

    DWORD nameLength = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, subjectName.c_str(), CERT_X500_NAME_STR, nullptr, nullptr,
                        &nameLength, nullptr) || nameLength == 0) {
        *error = windowsError(QStringLiteral("编码证书主题"), GetLastError());
        cleanup();
        return false;
    }
    QByteArray nameBuffer(int(nameLength), '\0');
    if (!CertStrToNameW(X509_ASN_ENCODING, subjectName.c_str(), CERT_X500_NAME_STR, nullptr,
                        reinterpret_cast<BYTE *>(nameBuffer.data()), &nameLength, nullptr)) {
        *error = windowsError(QStringLiteral("编码证书主题"), GetLastError());
        cleanup();
        return false;
    }
    CERT_NAME_BLOB subject{ nameLength, reinterpret_cast<BYTE *>(nameBuffer.data()) };

    CRYPT_KEY_PROV_INFO providerInfo{};
    providerInfo.pwszContainerName = const_cast<LPWSTR>(container.c_str());
    providerInfo.pwszProvName = const_cast<LPWSTR>(MS_ENH_RSA_AES_PROV_W);
    providerInfo.dwProvType = PROV_RSA_AES;
    providerInfo.dwKeySpec = AT_SIGNATURE;

    CRYPT_ALGORITHM_IDENTIFIER signatureAlgorithm{};
    signatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    const QDateTime now = QDateTime::currentDateTimeUtc();
    SYSTEMTIME startTime = toSystemTime(now.addDays(-1));
    SYSTEMTIME endTime = toSystemTime(now.addYears(kValidityYears));

    PCCERT_CONTEXT context = CertCreateSelfSignCertificate(provider, &subject, 0, &providerInfo,
                                                           &signatureAlgorithm, &startTime, &endTime, nullptr);
    if (!context) {
        *error = windowsError(QStringLiteral("签发自签证书"), GetLastError());
        cleanup();
        return false;
    }
    *certificateDer = QByteArray(reinterpret_cast<const char *>(context->pbCertEncoded),
                                 int(context->cbCertEncoded));
    CertFreeCertificateContext(context);

    DWORD blobLength = 0;
    if (!CryptExportKey(key, 0, PRIVATEKEYBLOB, 0, nullptr, &blobLength) || blobLength == 0) {
        *error = windowsError(QStringLiteral("导出私钥"), GetLastError());
        cleanup();
        return false;
    }
    QByteArray blob(int(blobLength), '\0');
    if (!CryptExportKey(key, 0, PRIVATEKEYBLOB, 0, reinterpret_cast<BYTE *>(blob.data()), &blobLength)) {
        *error = windowsError(QStringLiteral("导出私钥"), GetLastError());
        cleanup();
        return false;
    }
    blob.resize(int(blobLength));

    *pkcs8 = pkcs8FromPrivateBlob(blob, error);
    const bool ok = !pkcs8->isEmpty();
    cleanup();
    return ok;
}

} // namespace

QString fingerprintOf(const QSslCertificate &certificate)
{
    if (certificate.isNull())
        return {};
    return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex());
}

Identity loadOrCreateIdentity(const QString &dirPath, const QString &commonName)
{
    Identity identity;
    if (dirPath.isEmpty()) {
        identity.error = QStringLiteral("身份目录路径为空");
        return identity;
    }

    const QDir directory(dirPath);
    identity.certificatePath = directory.filePath(QString::fromLatin1(kCertificateFile));
    identity.privateKeyPath = directory.filePath(QString::fromLatin1(kPrivateKeyFile));

    if (QFile::exists(identity.certificatePath) && QFile::exists(identity.privateKeyPath)) {
        QFile certificateFile(identity.certificatePath);
        QFile keyFile(identity.privateKeyPath);
        if (!certificateFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
            identity.error = QStringLiteral("身份文件无法读取：%1").arg(directory.absolutePath());
            return identity;
        }
        identity.certificate = QSslCertificate(certificateFile.readAll(), QSsl::Pem);
        identity.privateKey = QSslKey(keyFile.readAll(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    } else {
        if (!QDir().mkpath(directory.absolutePath())) {
            identity.error = QStringLiteral("无法创建身份目录：%1").arg(directory.absolutePath());
            return identity;
        }

        QByteArray certificateDer;
        QByteArray pkcs8;
        if (!generateIdentity(commonName.isEmpty() ? QString::fromLatin1(kDefaultCommonName) : commonName,
                              &certificateDer, &pkcs8, &identity.error))
            return identity;

        identity.certificate = QSslCertificate(certificateDer, QSsl::Der);
        identity.privateKey = QSslKey(pkcs8, QSsl::Rsa, QSsl::Der, QSsl::PrivateKey);
        if (identity.certificate.isNull() || identity.privateKey.isNull()) {
            identity.error = QStringLiteral("当前 TLS 后端无法加载生成的证书或私钥");
            return identity;
        }

        QFile certificateFile(identity.certificatePath);
        QFile keyFile(identity.privateKeyPath);
        if (!certificateFile.open(QIODevice::WriteOnly) || !keyFile.open(QIODevice::WriteOnly)) {
            identity.error = QStringLiteral("无法写入身份文件：%1").arg(directory.absolutePath());
            return identity;
        }
        certificateFile.write(pemEncode(certificateDer, "CERTIFICATE"));
        keyFile.write(pemEncode(pkcs8, "PRIVATE KEY"));
    }

    if (identity.certificate.isNull()) {
        identity.error = QStringLiteral("证书解析失败：%1").arg(identity.certificatePath);
        return identity;
    }
    if (identity.privateKey.isNull()) {
        identity.error = QStringLiteral("私钥解析失败：%1").arg(identity.privateKeyPath);
        return identity;
    }

    identity.fingerprint = fingerprintOf(identity.certificate);
    return identity;
}

} // namespace Sync::Net
