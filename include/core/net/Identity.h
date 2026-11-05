#pragma once

#include <QByteArray>
#include <QString>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslKey>

namespace Sync::Net {

/// 设备身份：自签 RSA-2048 证书 + 配套私钥，落盘为 PEM。
/// 直连通道用它做双向 mTLS 的本地证书（设计 §4.1）。
/// 不用 ECDSA：Schannel 后端把证书+私钥打 PKCS#12 时只接受 RSA/DSA（qsslsocket_qt.cpp:140 断言）。
struct Identity
{
    QSslCertificate certificate;
    QSslKey privateKey;
    QString fingerprint;        ///< 证书 DER 的 SHA-256，小写十六进制
    QString certificatePath;
    QString privateKeyPath;
    QString error;

    bool isValid() const { return !certificate.isNull() && !privateKey.isNull(); }
};

/// 证书 DER 的 SHA-256，小写十六进制；空证书返回空串。
QString fingerprintOf(const QSslCertificate &certificate);

/// 已存在则读取，不存在则生成并写入 dirPath。私钥为明文 PEM，权限依赖目录 ACL。
Identity loadOrCreateIdentity(const QString &dirPath, const QString &commonName);

} // namespace Sync::Net
