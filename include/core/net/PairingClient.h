#pragma once

#include <QByteArray>
#include <QString>

#include "core/net/NetTransport.h"

namespace Sync::Net {

/// 配对结果：成功时 `deviceSecret` + `serverFingerprint` 都要落盘，
/// 后者此后每次连接都要比对（首次配对是 TOFU，见设计 §4.2）。
struct PairingOutcome
{
    bool       ok = false;
    QString    deviceId;
    QString    serverFingerprint;
    QByteArray deviceSecret;
    QString    error;
};

/// 用 6 位配对码跟 host 配对，成功返回设备口令。
///
/// 阻塞式（与 NetTransport 一样用 waitFor*），只能在没有事件循环的线程里调用；
/// 时间有限、一次性，配对成功后**必须**断开重连一次才进入正常同步。
PairingOutcome pairWithHost(const NetPeer &peer, const QString &code,
                            const QString &clientName, const QString &clientFingerprint);

} // namespace Sync::Net
