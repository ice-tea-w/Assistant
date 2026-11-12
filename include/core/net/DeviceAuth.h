#pragma once

#include <QByteArray>
#include <QString>

namespace Sync::Net {

/// 挑战应答用的设备口令（配对时生成，32 字节随机）。
constexpr int kDeviceSecretSize = 32;
/// 服务端每次连接发的随机挑战。
constexpr int kChallengeSize = 32;

/// HMAC-SHA256，供挑战应答与测试向量使用。
QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);

/// 挑战应答摘要 = HMAC-SHA256(deviceSecret, "ASYN-AUTH-V1" ‖ deviceId ‖ nonce ‖ 服务端证书指纹)。
/// 三个字段都带长度前缀，避免拼接歧义；改动任一字段都会改变结果。
QByteArray challengeResponse(const QByteArray &deviceSecret, const QByteArray &nonce,
                             const QString &deviceId, const QString &serverFingerprint);

/// 定长比较，长度不等直接 false；不提前返回，避免按字节泄露。
bool verifyChallengeResponse(const QByteArray &expected, const QByteArray &actual);

/// 配对证明 = HMAC-SHA256(6 位配对码, "ASYN-PAIR-V1" ‖ nonce ‖ 服务端指纹 ‖ 客户端指纹)。
/// 配对码由 host 显示、用户口述给客户端，走带外通道；TLS 只负责传输不被窃听。
QByteArray pairingProof(const QString &code, const QByteArray &nonce,
                        const QString &serverFingerprint, const QString &clientFingerprint);

/// 6 位配对码（含前导零），共 10^6 种。
QString randomPairingCode();

QByteArray randomBytes(int size);

} // namespace Sync::Net
