#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace Sync::Net {

constexpr quint8 kVersion = 0x02;
constexpr int kHeaderSize = 16;
constexpr quint32 kMaxPayload = 64 * 1024;
constexpr char kMagic[4] = {'A', 'S', 'Y', 'N'};

enum class FrameType : quint8 {
    Hello = 0x01,
    Challenge = 0x02,
    Auth = 0x03,
    HelloAck = 0x04,
    // 配对（设计 §4.2）：只有 host 打开配对窗口时才接受，成功后立刻断开重连认证
    PairHello = 0x05,
    PairChallenge = 0x06,
    PairProof = 0x07,
    PairResult = 0x08,
    List = 0x10,
    ListResp = 0x11,
    Get = 0x20,
    Chunk = 0x21,
    End = 0x22,
    Put = 0x30,
    PutChunk = 0x31,
    PutAck = 0x32,
    Delete = 0x40,
    DeleteAck = 0x41,
    Lock = 0x50,
    LockAck = 0x51,
    Unlock = 0x52,
    UnlockAck = 0x53,
    Ping = 0x60,
    Pong = 0x61,
    Error = 0xFF
};

enum FrameFlag : quint16 {
    FlagJson = 0x0001,
    FlagBlob = 0x0002,
    FlagLast = 0x0004
};

struct Frame {
    FrameType type = FrameType::Error;
    quint16 flags = 0;
    quint32 reqId = 0;
    QByteArray payload;
};

QByteArray encodeFrame(const Frame &frame);

class FrameDecoder {
public:
    enum class Status { Frame, NeedMore, Error };

    void append(const QByteArray &data);
    Status next(Frame *out, QString *error = nullptr);
    void reset();
    int buffered() const { return m_buf.size(); }
    bool failed() const { return m_failed; }

private:
    void fail(const QString &message);

    QByteArray m_buf;
    QString m_error;
    bool m_failed = false;
};

} // namespace Sync::Net
