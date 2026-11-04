#include "core/sync/ConflictResolver.h"

#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>

namespace Sync {

bool sameState(const FileState &a, const FileState &b)
{
    if (a.exists() != b.exists())
        return false;
    if (!a.exists())              // 两边都不存在
        return true;

    // 优先比哈希：mtime 会被复制/同步工具改写，不可靠
    if (!a.hash.isEmpty() && !b.hash.isEmpty())
        return a.hash.compare(b.hash, Qt::CaseInsensitive) == 0;

    return a.size == b.size && a.mtime == b.mtime;
}

SyncAction decide(const FileState &L, const FileState &S, const FileState &R)
{
    const bool localChanged  = !sameState(L, S);
    const bool remoteChanged = !sameState(R, S);

    // 情况 1：双方都没动
    if (!localChanged && !remoteChanged)
        return SyncAction::None;

    // 情况 2：只有本地动了
    if (localChanged && !remoteChanged) {
        if (!L.exists() && S.exists())
            return SyncAction::DeleteRemote;   // 本地删除
        return SyncAction::Upload;             // 本地新增或修改
    }

    // 情况 3：只有对端动了
    if (!localChanged && remoteChanged) {
        if (!R.exists() && S.exists())
            return SyncAction::DeleteLocal;    // 对端删除
        return SyncAction::Download;           // 对端新增或修改
    }

    // 情况 4：双方都动了 —— 内容是否恰好一致？
    if (sameState(L, R))
        return SyncAction::UpdateBaseline;

    // 情况 5：真冲突
    return SyncAction::Conflict;
}

QString makeConflictName(const QString &originalName,
                         const QString &peerDeviceName,
                         qint64 detectedAtEpoch)
{
    const QFileInfo fi(originalName);
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix();

    const QString stamp =
        QDateTime::fromSecsSinceEpoch(detectedAtEpoch).toString(QStringLiteral("yyyyMMdd-HHmmss"));

    // 设备名可能含非法文件名字符，做最小净化
    QString peer = peerDeviceName;
    peer.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    if (peer.isEmpty())
        peer = QStringLiteral("对端");

    QString name = QStringLiteral("%1 (冲突副本 来自%2 %3)").arg(base, peer, stamp);
    if (!suffix.isEmpty())
        name += QLatin1Char('.') + suffix;
    return name;
}

} // namespace Sync
