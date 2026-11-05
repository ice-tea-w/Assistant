#include "core/net/SyncServer.h"

#include "core/net/DeviceAuth.h"
#include "core/net/SyncFrame.h"
#include "core/sync/PathGuard.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QSet>
#include <QTcpServer>
#include <QTimer>

#include <algorithm>

namespace Sync::Net {

namespace {

constexpr int kDefaultPage = 200;
constexpr int kMaxPage     = 500;

QByteArray toJson(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

bool isJson(const QByteArray &payload)
{
    return payload.startsWith('{');
}

} // namespace

/// 不是为了用 QSslServer 而用 QSslServer：它把"握手完成"的 socket 放进 pending 队列，
/// 而我们要在 adopt 时立刻挂上自己的 readyRead。这里自己接管 acceptor，握手过程完全可见。
class TlsAcceptor : public QTcpServer
{
public:
    explicit TlsAcceptor(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
    }

    QSslConfiguration cfg;
    std::function<void(QSslSocket *)> onEncrypted;

    void incomingConnection(qintptr fd) override
    {
        auto *socket = new QSslSocket(this);
        socket->setSslConfiguration(cfg);
        if (!socket->setSocketDescriptor(fd)) {
            socket->deleteLater();
            return;
        }
        connect(socket, &QSslSocket::encrypted, this, [this, socket] {
            if (onEncrypted)
                onEncrypted(socket);
        });
        connect(socket, &QSslSocket::disconnected, this, [socket] { socket->deleteLater(); });
        socket->startServerEncryption();
    }
};

class SyncServer::Connection
{
public:
    enum class State { AwaitHello, AwaitAuth, Ready };

    QSslSocket  *socket = nullptr;
    FrameDecoder decoder;
    State        state = State::AwaitHello;
    QByteArray   nonce;
    QString      deviceId;
    QString      peer;      // 对端 IP，认证失败限速按它计数

    // Put 的落盘状态：先写 .assistant-part，校验通过才改名成正式文件
    QString      putRelPath;
    QString      putExpectedHash;
    qint64       putExpectedSize = -1;
    qint64       putReceived     = 0;
    std::unique_ptr<QFile> putFile;
    std::unique_ptr<QCryptographicHash> putHasher;
};

SyncServer::SyncServer(const Identity &identity, SecretLookup secretLookup, QObject *parent)
    : QObject(parent)
    , m_identity(identity)
    , m_secretLookup(std::move(secretLookup))
{
    // 索引写盘做 2 秒合并：一次同步里 Put 几十个文件，不必每个都刷一次 JSON
    m_indexTimer = new QTimer(this);
    m_indexTimer->setSingleShot(true);
    m_indexTimer->setInterval(2000);
    connect(m_indexTimer, &QTimer::timeout, this, [this] {
        if (m_indexDirty)
            saveIndex();
    });
}

SyncServer::~SyncServer()
{
    close();
}

bool SyncServer::listen(quint16 port, const QString &sharedRoot)
{
    close();

    QDir root(sharedRoot);
    if (sharedRoot.isEmpty() || !root.exists()) {
        m_lastError = QStringLiteral("共享根目录不存在：%1").arg(sharedRoot);
        return false;
    }
    m_sharedRoot = root.absolutePath();
    loadIndex();

    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setLocalCertificate(m_identity.certificate);
    cfg.setPrivateKey(m_identity.privateKey);
    cfg.setPeerVerifyMode(QSslSocket::QueryPeer);
    cfg.setProtocol(QSsl::TlsV1_2OrLater);

    auto acceptor = std::make_unique<TlsAcceptor>();
    acceptor->cfg = cfg;
    acceptor->onEncrypted = [this](QSslSocket *socket) { adoptSocket(socket); };

    if (!acceptor->listen(QHostAddress::Any, port)) {
        m_lastError = QStringLiteral("监听失败：%1").arg(acceptor->errorString());
        return false;
    }
    m_server = std::move(acceptor);
    return true;
}

void SyncServer::close()
{
    for (auto it = m_connections.begin(); it != m_connections.end(); ++it) {
        if (it.value()->socket)
            it.value()->socket->disconnect(this);
    }
    m_connections.clear();
    if (m_server) {
        m_server->close();
        m_server.reset();
    }
    if (m_indexDirty && !m_sharedRoot.isEmpty())
        saveIndex();
    m_sharedRoot.clear();
}

quint16 SyncServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

QString SyncServer::lastError() const
{
    return m_lastError;
}

QString SyncServer::sharedRoot() const
{
    return m_sharedRoot;
}

int SyncServer::readyConnections() const
{
    int n = 0;
    for (const auto &conn : m_connections) {
        if (conn->state == Connection::State::Ready)
            ++n;
    }
    return n;
}

void SyncServer::setAuthLockPolicy(int maxFailures, int lockMs)
{
    m_maxAuthFailures = maxFailures > 0 ? maxFailures : 1;
    m_authLockMs      = lockMs > 0 ? lockMs : 1;
}

void SyncServer::adoptSocket(QSslSocket *socket)
{
    auto conn = std::make_shared<Connection>();
    conn->socket = socket;
    conn->peer   = socket->peerAddress().toString();
    m_connections.insert(socket, conn);

    connect(socket, &QSslSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
    connect(socket, &QSslSocket::disconnected, this, [this, socket] { onDisconnected(socket); });
    connect(socket, &QSslSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) { onDisconnected(socket); });

    // 握手完成时对端可能已经把 Hello 发出来了，先处理一次存量数据
    if (socket->bytesAvailable() > 0)
        onReadyRead(socket);
}

void SyncServer::onDisconnected(QSslSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;
    dropPartFile(*it.value());   // 半途断开的 Put 不留下 .assistant-part
    m_connections.erase(it);
    socket->deleteLater();
}

void SyncServer::onReadyRead(QSslSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;

    Connection &conn = *it.value();
    conn.decoder.append(socket->readAll());

    while (true) {
        Frame frame;
        QString error;
        const FrameDecoder::Status status = conn.decoder.next(&frame, &error);
        if (status == FrameDecoder::Status::NeedMore)
            return;
        if (status == FrameDecoder::Status::Error) {
            sendError(conn, 0, QStringLiteral("PROTOCOL"), error);
            socket->disconnectFromHost();
            return;
        }
        handleFrame(conn, frame);
        if (m_connections.find(socket) == m_connections.end())
            return;   // 处理过程中被断开（认证失败）
    }
}

void SyncServer::handleFrame(Connection &conn, const Frame &frame)
{
    // 锁定期内连 Hello 都不回 Challenge，免得给暴力破解者一个可测的 oracle
    if (isPeerLocked(conn.peer)) {
        sendError(conn, frame.reqId, QStringLiteral("AUTH_LOCKED"),
                  QStringLiteral("认证失败次数过多，请稍后再试"));
        conn.socket->disconnectFromHost();
        return;
    }

    if (conn.state == Connection::State::AwaitHello) {
        if (frame.type != FrameType::Hello) {
            sendError(conn, frame.reqId, QStringLiteral("EXPECTED_HELLO"),
                      QStringLiteral("首帧必须是 Hello"));
            conn.socket->disconnectFromHost();
            return;
        }
        handleHello(conn, frame);
        return;
    }

    if (conn.state == Connection::State::AwaitAuth) {
        if (frame.type != FrameType::Auth) {
            sendError(conn, frame.reqId, QStringLiteral("EXPECTED_AUTH"),
                      QStringLiteral("Hello 之后必须是 Auth"));
            conn.socket->disconnectFromHost();
            return;
        }
        handleAuth(conn, frame);
        return;
    }

    switch (frame.type) {
    case FrameType::List:
        handleList(conn, frame);
        break;
    case FrameType::Get:
        handleGet(conn, frame);
        break;
    case FrameType::Put:
        handlePut(conn, frame);
        break;
    case FrameType::PutChunk:
        handlePutChunk(conn, frame);
        break;
    case FrameType::End:
        handlePutEnd(conn, frame);
        break;
    case FrameType::Delete:
        handleDelete(conn, frame);
        break;
    case FrameType::Ping:
        send(conn, static_cast<quint8>(FrameType::Pong), frame.reqId, QByteArray());
        break;
    default:
        sendError(conn, frame.reqId, QStringLiteral("UNSUPPORTED"),
                  QStringLiteral("尚未实现的帧类型"));
        break;
    }
}

void SyncServer::handleHello(Connection &conn, const Frame &frame)
{
    const QJsonObject obj = QJsonDocument::fromJson(frame.payload).object();
    const QString deviceId = obj.value(QStringLiteral("deviceId")).toString();
    if (deviceId.isEmpty()) {
        sendError(conn, frame.reqId, QStringLiteral("BAD_HELLO"),
                  QStringLiteral("Hello 缺 deviceId"));
        conn.socket->disconnectFromHost();
        return;
    }

    conn.deviceId = deviceId;
    conn.nonce    = randomBytes(kChallengeSize);
    conn.state    = Connection::State::AwaitAuth;

    send(conn, static_cast<quint8>(FrameType::Challenge), frame.reqId, conn.nonce);
}

void SyncServer::handleAuth(Connection &conn, const Frame &frame)
{
    const QByteArray secret = m_secretLookup ? m_secretLookup(conn.deviceId) : QByteArray();
    const QByteArray expected = secret.isEmpty()
        ? QByteArray()
        : challengeResponse(secret, conn.nonce, conn.deviceId, m_identity.fingerprint);

    if (expected.isEmpty() || !verifyChallengeResponse(expected, frame.payload)) {
        recordAuthFailure(conn.peer);
        emit authFailed(conn.deviceId);
        sendError(conn, frame.reqId, QStringLiteral("AUTH_FAILED"),
                  QStringLiteral("设备未配对或口令不匹配"));
        conn.socket->disconnectFromHost();
        return;
    }

    conn.nonce.fill('\0');
    conn.nonce.clear();
    conn.state = Connection::State::Ready;
    clearAuthFailures(conn.peer);

    QJsonObject ack;
    ack.insert(QStringLiteral("serverFingerprint"), m_identity.fingerprint);
    ack.insert(QStringLiteral("protocol"), kVersion);
    send(conn, static_cast<quint8>(FrameType::HelloAck), frame.reqId, toJson(ack));

    emit clientAuthenticated(conn.deviceId);
}

void SyncServer::handleList(Connection &conn, const Frame &frame)
{
    const QJsonObject request = QJsonDocument::fromJson(frame.payload).object();
    int cursor = request.value(QStringLiteral("cursor")).toInt(0);
    int limit  = request.value(QStringLiteral("limit")).toInt(kDefaultPage);
    if (cursor < 0)
        cursor = 0;
    if (limit <= 0 || limit > kMaxPage)
        limit = kDefaultPage;

    const QList<ServedFile> all = collectFiles();

    QJsonArray files;
    int index = cursor;
    for (; index < all.size() && files.size() < limit; ++index) {
        const ServedFile &f = all.at(index);
        QJsonObject item;
        item.insert(QStringLiteral("path"), f.relPath);
        item.insert(QStringLiteral("size"), double(f.size));
        item.insert(QStringLiteral("mtime"), double(f.mtime));
        item.insert(QStringLiteral("hash"), f.hash);
        files.append(item);
    }

    QJsonObject response;
    response.insert(QStringLiteral("files"), files);
    response.insert(QStringLiteral("nextCursor"), index);
    response.insert(QStringLiteral("hasMore"), index < all.size());

    send(conn, static_cast<quint8>(FrameType::ListResp), frame.reqId, toJson(response));
}

void SyncServer::handleGet(Connection &conn, const Frame &frame)
{
    const QString relPath =
        QJsonDocument::fromJson(frame.payload).object().value(QStringLiteral("path")).toString();
    if (!Sync::PathGuard::isSafeRelativePath(relPath)) {
        sendError(conn, frame.reqId, QStringLiteral("BAD_PATH"),
                  QStringLiteral("非法相对路径：%1").arg(relPath));
        return;
    }

    QFile file(Sync::PathGuard::resolveUnder(m_sharedRoot, relPath));
    if (!file.open(QIODevice::ReadOnly)) {
        sendError(conn, frame.reqId, QStringLiteral("NOT_FOUND"),
                  QStringLiteral("对端没有这个文件：%1").arg(relPath));
        return;
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kMaxPayload);
        if (chunk.isEmpty())
            break;
        hasher.addData(chunk);

        Frame data;
        data.type    = FrameType::Chunk;
        data.flags   = quint16(FrameFlag::FlagBlob);
        data.reqId   = frame.reqId;
        data.payload = chunk;
        conn.socket->write(encodeFrame(data));
    }

    QJsonObject end;
    end.insert(QStringLiteral("hash"), QString::fromLatin1(hasher.result().toHex()));
    end.insert(QStringLiteral("size"), double(file.size()));
    send(conn, static_cast<quint8>(FrameType::End), frame.reqId, toJson(end));
}

void SyncServer::dropPartFile(Connection &conn)
{
    if (conn.putFile) {
        const QString partPath = conn.putFile->fileName();
        conn.putFile->close();
        conn.putFile.reset();
        QFile::remove(partPath);
    }
    conn.putHasher.reset();
    conn.putRelPath.clear();
    conn.putExpectedHash.clear();
    conn.putExpectedSize = -1;
    conn.putReceived     = 0;
}

void SyncServer::handlePut(Connection &conn, const Frame &frame)
{
    const QJsonObject obj = QJsonDocument::fromJson(frame.payload).object();
    const QString relPath = obj.value(QStringLiteral("path")).toString();
    const QString hash    = obj.value(QStringLiteral("hash")).toString();

    if (!Sync::PathGuard::isSafeRelativePath(relPath)) {
        sendError(conn, frame.reqId, QStringLiteral("BAD_PATH"),
                  QStringLiteral("非法相对路径：%1").arg(relPath));
        return;
    }
    if (conn.putFile) {
        sendError(conn, frame.reqId, QStringLiteral("PUT_BUSY"),
                  QStringLiteral("上一个 Put 还没结束"));
        return;
    }

    // 目标子目录可能还不存在，而 resolveUnder 要求父目录已存在（它要解引用
    // junction 后才能复核前缀）。所以先逐段把父目录准备出来，再拼文件名。
    const QFileInfo relInfo(relPath);
    const QString parentRel = relInfo.path();
    QString parentAbs;
    if (parentRel.isEmpty() || parentRel == QStringLiteral("."))
        parentAbs = QFileInfo(QDir(m_sharedRoot).absolutePath()).canonicalFilePath();
    else
        parentAbs = Sync::PathGuard::ensureDirUnder(m_sharedRoot, parentRel);

    if (parentAbs.isEmpty()) {
        sendError(conn, frame.reqId, QStringLiteral("BAD_PATH"),
                  QStringLiteral("无法在共享根之下准备目录：%1").arg(relPath));
        return;
    }

    const QString absPath = QDir(parentAbs).absoluteFilePath(relInfo.fileName());

    auto file = std::make_unique<QFile>(absPath + QStringLiteral(".assistant-part"));
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sendError(conn, frame.reqId, QStringLiteral("WRITE_FAILED"),
                  QStringLiteral("无法写入：%1").arg(relPath));
        return;
    }

    conn.putRelPath      = relPath;
    conn.putExpectedHash = hash.toLower();
    conn.putExpectedSize = qint64(obj.value(QStringLiteral("size")).toDouble(-1));
    conn.putReceived     = 0;
    conn.putFile         = std::move(file);
    conn.putHasher       = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
}

void SyncServer::handlePutChunk(Connection &conn, const Frame &frame)
{
    if (!conn.putFile) {
        sendError(conn, frame.reqId, QStringLiteral("NO_PUT"),
                  QStringLiteral("收到数据块但没有进行中的 Put"));
        return;
    }
    if (conn.putFile->write(frame.payload) != frame.payload.size()) {
        const QString relPath = conn.putRelPath;
        dropPartFile(conn);
        sendError(conn, frame.reqId, QStringLiteral("WRITE_FAILED"),
                  QStringLiteral("写盘失败：%1").arg(relPath));
        return;
    }
    conn.putHasher->addData(frame.payload);
    conn.putReceived += frame.payload.size();
}

void SyncServer::handlePutEnd(Connection &conn, const Frame &frame)
{
    if (!conn.putFile) {
        sendError(conn, frame.reqId, QStringLiteral("NO_PUT"),
                  QStringLiteral("收到结束帧但没有进行中的 Put"));
        return;
    }

    const QString relPath = conn.putRelPath;
    const QString expected = conn.putExpectedHash;
    const qint64 expectedSize = conn.putExpectedSize;
    const QString actual = QString::fromLatin1(conn.putHasher->result().toHex());
    const qint64 received = conn.putReceived;
    const QString partPath = conn.putFile->fileName();

    conn.putFile->close();
    conn.putFile.reset();
    conn.putHasher.reset();
    conn.putRelPath.clear();
    conn.putExpectedHash.clear();
    conn.putExpectedSize = -1;
    conn.putReceived = 0;

    const bool sizeOk = expectedSize < 0 || received == expectedSize;
    const bool hashOk = expected.isEmpty() || actual == expected;
    if (!sizeOk || !hashOk) {
        QFile::remove(partPath);
        sendError(conn, frame.reqId, QStringLiteral("HASH_MISMATCH"),
                  QStringLiteral("校验失败：%1（大小 %2 / 哈希 %3）")
                      .arg(relPath)
                      .arg(sizeOk ? QStringLiteral("ok") : QStringLiteral("不一致"))
                      .arg(hashOk ? QStringLiteral("ok") : QStringLiteral("不一致")));
        return;
    }

    const QString absPath = Sync::PathGuard::resolveUnder(m_sharedRoot, relPath);
    QFile::remove(absPath);   // Windows 上 rename 不覆盖已存在文件
    if (!QFile::rename(partPath, absPath)) {
        QFile::remove(partPath);
        sendError(conn, frame.reqId, QStringLiteral("RENAME_FAILED"),
                  QStringLiteral("无法落盘：%1").arg(relPath));
        return;
    }
    CachedHash entry;
    entry.size  = received;
    entry.mtime = QFileInfo(absPath).lastModified().toSecsSinceEpoch();
    entry.hash  = actual;
    m_index.insert(relPath, entry);
    markIndexDirty();

    QJsonObject ack;
    ack.insert(QStringLiteral("ok"), true);
    ack.insert(QStringLiteral("hash"), actual);
    send(conn, static_cast<quint8>(FrameType::PutAck), frame.reqId, toJson(ack));
}

void SyncServer::handleDelete(Connection &conn, const Frame &frame)
{
    const QString relPath =
        QJsonDocument::fromJson(frame.payload).object().value(QStringLiteral("path")).toString();
    if (!Sync::PathGuard::isSafeRelativePath(relPath)) {
        sendError(conn, frame.reqId, QStringLiteral("BAD_PATH"),
                  QStringLiteral("非法相对路径：%1").arg(relPath));
        return;
    }

    const QString absPath = Sync::PathGuard::resolveUnder(m_sharedRoot, relPath);
    if (!QFile::exists(absPath)) {
        // 已经不在了也算成功：同步是幂等的
        QJsonObject ack;
        ack.insert(QStringLiteral("ok"), true);
        ack.insert(QStringLiteral("existed"), false);
        send(conn, static_cast<quint8>(FrameType::DeleteAck), frame.reqId, toJson(ack));
        return;
    }

    const QString trashDir = QDir(m_sharedRoot).filePath(QStringLiteral(".sync/trash"));
    if (!QDir().mkpath(trashDir)) {
        sendError(conn, frame.reqId, QStringLiteral("TRASH_FAILED"),
                  QStringLiteral("无法创建回收目录"));
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString target = QDir(trashDir).filePath(stamp + QLatin1Char('-') + QFileInfo(relPath).fileName());
    if (!QFile::rename(absPath, target)) {
        sendError(conn, frame.reqId, QStringLiteral("DELETE_FAILED"),
                  QStringLiteral("无法移入回收目录：%1").arg(relPath));
        return;
    }
    m_index.remove(relPath);
    markIndexDirty();

    QJsonObject ack;
    ack.insert(QStringLiteral("ok"), true);
    ack.insert(QStringLiteral("existed"), true);
    send(conn, static_cast<quint8>(FrameType::DeleteAck), frame.reqId, toJson(ack));
}

QList<ServedFile> SyncServer::collectFiles()
{
    QList<ServedFile> out;
    if (m_sharedRoot.isEmpty())
        return out;

    const QDir rootDir(m_sharedRoot);
    QSet<QString> seen;

    QDirIterator it(m_sharedRoot, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString absPath = it.next();
        const QFileInfo info = it.fileInfo();

        const QString relPath = rootDir.relativeFilePath(info.absoluteFilePath());
        if (relPath.startsWith(QStringLiteral(".sync") + QLatin1Char('/'))
            || relPath.startsWith(QStringLiteral(".assistant-sync") + QLatin1Char('/'))
            || relPath.contains(QStringLiteral("/.assistant-part"))) {
            continue;
        }

        ServedFile f;
        f.relPath = relPath;
        f.size    = info.size();
        f.mtime   = info.lastModified().toSecsSinceEpoch();

        const CachedHash cached = m_index.value(relPath);
        if (!cached.hash.isEmpty() && cached.size == f.size && cached.mtime == f.mtime) {
            f.hash = cached.hash;
        } else {
            f.hash = hashOf(info.absoluteFilePath());
            if (!f.hash.isEmpty()) {
                CachedHash entry;
                entry.size  = f.size;
                entry.mtime = f.mtime;
                entry.hash  = f.hash;
                m_index.insert(relPath, entry);
                markIndexDirty();
            }
        }

        seen.insert(relPath);
        out.append(f);
    }

    // 在共享根之外被删掉的文件不会通知我们，遍历时顺手把陈旧条目摘掉
    for (auto entry = m_index.begin(); entry != m_index.end();) {
        if (seen.contains(entry.key())) {
            ++entry;
        } else {
            entry = m_index.erase(entry);
            markIndexDirty();
        }
    }

    std::sort(out.begin(), out.end(), [](const ServedFile &a, const ServedFile &b) {
        return a.relPath < b.relPath;
    });
    return out;
}

QString SyncServer::hashOf(const QString &absPath)
{
    QFile file(absPath);
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    if (!hasher.addData(&file))
        return QString();

    return QString::fromLatin1(hasher.result().toHex());
}

void SyncServer::send(Connection &conn, quint8 type, quint32 reqId, const QByteArray &payload)
{
    if (!conn.socket || conn.socket->state() != QAbstractSocket::ConnectedState)
        return;

    Frame frame;
    frame.type    = static_cast<FrameType>(type);
    frame.flags   = isJson(payload) ? quint16(FrameFlag::FlagJson) : quint16(FrameFlag::FlagBlob);
    frame.reqId   = reqId;
    frame.payload = payload;
    conn.socket->write(encodeFrame(frame));
}

void SyncServer::sendError(Connection &conn, quint32 reqId, const QString &code, const QString &message)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    send(conn, static_cast<quint8>(FrameType::Error), reqId, toJson(error));
}

QString SyncServer::indexPath() const
{
    return QDir(m_sharedRoot).filePath(QStringLiteral(".assistant-sync/index.json"));
}

void SyncServer::loadIndex()
{
    m_index.clear();
    m_indexDirty = false;

    QFile file(indexPath());
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QJsonObject files =
        QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("files")).toObject();
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        CachedHash cached;
        cached.size  = qint64(entry.value(QStringLiteral("size")).toDouble(-1));
        cached.mtime = qint64(entry.value(QStringLiteral("mtime")).toDouble(0));
        cached.hash  = entry.value(QStringLiteral("hash")).toString();
        if (cached.size < 0 || cached.hash.isEmpty())
            continue;
        m_index.insert(it.key(), cached);
    }
}

bool SyncServer::saveIndex()
{
    if (m_sharedRoot.isEmpty())
        return false;

    QJsonObject files;
    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
        QJsonObject entry;
        entry.insert(QStringLiteral("size"), double(it->size));
        entry.insert(QStringLiteral("mtime"), double(it->mtime));
        entry.insert(QStringLiteral("hash"), it->hash);
        files.insert(it.key(), entry);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("files"), files);

    const QString path = indexPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;

    // 先写同目录临时文件再改名：崩在半路最多丢缓存，不会留下半个 JSON
    const QString tmpPath = path + QStringLiteral(".tmp");
    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    tmp.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    tmp.close();

    QFile::remove(path);   // Windows 上 rename 不覆盖已存在文件
    if (!QFile::rename(tmpPath, path)) {
        QFile::remove(tmpPath);
        return false;
    }

    m_indexDirty = false;
    return true;
}

void SyncServer::markIndexDirty()
{
    m_indexDirty = true;
    if (m_indexTimer && !m_indexTimer->isActive())
        m_indexTimer->start();
}

bool SyncServer::isPeerLocked(const QString &peer) const
{
    const auto it = m_failures.constFind(peer);
    if (it == m_failures.constEnd())
        return false;
    return it->lockedUntilMs > QDateTime::currentMSecsSinceEpoch();
}

void SyncServer::recordAuthFailure(const QString &peer)
{
    FailureRecord &record = m_failures[peer];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (record.lockedUntilMs != 0 && now >= record.lockedUntilMs) {
        // 锁过期后重新开始计数，否则下一次失败会立刻再锁一轮
        record.count         = 0;
        record.lockedUntilMs = 0;
    }
    ++record.count;
    if (record.count >= m_maxAuthFailures)
        record.lockedUntilMs = now + m_authLockMs;
}

void SyncServer::clearAuthFailures(const QString &peer)
{
    m_failures.remove(peer);
}

} // namespace Sync::Net
