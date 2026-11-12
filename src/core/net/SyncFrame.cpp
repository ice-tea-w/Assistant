#include "core/net/SyncFrame.h"

#include <QtEndian>

namespace Sync::Net {

QByteArray encodeFrame(const Frame &frame)
{
    if (frame.payload.size() > int(kMaxPayload))
        return {};

    QByteArray bytes;
    bytes.reserve(kHeaderSize + frame.payload.size());
    bytes.append(kMagic, 4);
    bytes.append(char(kVersion));
    bytes.append(char(quint8(frame.type)));

    const quint16 flags = qToLittleEndian(frame.flags);
    bytes.append(reinterpret_cast<const char *>(&flags), 2);
    const quint32 reqId = qToLittleEndian(frame.reqId);
    bytes.append(reinterpret_cast<const char *>(&reqId), 4);
    const quint32 length = qToLittleEndian(quint32(frame.payload.size()));
    bytes.append(reinterpret_cast<const char *>(&length), 4);
    bytes.append(frame.payload);
    return bytes;
}

void FrameDecoder::append(const QByteArray &data)
{
    if (!m_failed)
        m_buf.append(data);
}

FrameDecoder::Status FrameDecoder::next(Frame *out, QString *error)
{
    if (error)
        error->clear();
    if (m_failed) {
        if (error)
            *error = m_error;
        return Status::Error;
    }
    if (m_buf.size() < kHeaderSize)
        return Status::NeedMore;

    if (m_buf.left(4) != QByteArray(kMagic, 4)) {
        fail(QStringLiteral("帧头 magic 不是 ASYN"));
        if (error)
            *error = m_error;
        return Status::Error;
    }

    const quint8 version = quint8(m_buf.at(4));
    if (version != kVersion) {
        fail(QStringLiteral("不支持的协议版本 0x%1（期望 0x%2）")
                 .arg(version, 2, 16, QLatin1Char('0'))
                 .arg(kVersion, 2, 16, QLatin1Char('0')));
        if (error)
            *error = m_error;
        return Status::Error;
    }

    const uchar *base = reinterpret_cast<const uchar *>(m_buf.constData());
    const quint32 length = qFromLittleEndian<quint32>(base + 12);
    if (length > kMaxPayload) {
        fail(QStringLiteral("载荷长度 %1 超过上限 %2").arg(length).arg(kMaxPayload));
        if (error)
            *error = m_error;
        return Status::Error;
    }
    if (m_buf.size() < kHeaderSize + int(length))
        return Status::NeedMore;

    if (!out) {
        fail(QStringLiteral("内部错误：next() 未提供输出参数"));
        if (error)
            *error = m_error;
        return Status::Error;
    }

    out->type = FrameType(m_buf.at(5));
    out->flags = qFromLittleEndian<quint16>(base + 6);
    out->reqId = qFromLittleEndian<quint32>(base + 8);
    out->payload = m_buf.mid(kHeaderSize, int(length));
    m_buf.remove(0, kHeaderSize + int(length));
    return Status::Frame;
}

void FrameDecoder::reset()
{
    m_buf.clear();
    m_error.clear();
    m_failed = false;
}

void FrameDecoder::fail(const QString &message)
{
    m_failed = true;
    m_error = message;
}

} // namespace Sync::Net
