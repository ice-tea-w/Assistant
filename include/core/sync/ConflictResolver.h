#pragma once

#include <QString>

namespace Sync {

/// 一个文件在某一侧的状态。
struct FileState
{
    qint64  size  = -1;   // -1 = 文件不存在
    qint64  mtime = 0;    // Unix 秒
    QString hash;         // SHA-256；空 = 未计算

    bool exists() const { return size >= 0; }
};

enum class SyncAction
{
    None,            // 无需动作
    Upload,          // 本地改动 → 上传
    Download,        // 对端改动 → 下载
    UpdateBaseline,  // 两端改成同一内容 → 只更新基线
    Conflict,        // 两端改成不同内容 → 生成冲突副本
    DeleteLocal,     // 对端删除且本地未改 → 本地移入回收目录
    DeleteRemote     // 本地删除且对端未改 → 请求对端移入回收目录
};

/// 判定两个状态是否"相同"。
///
/// 规则：双方都有 hash 时只比 hash；任一方缺 hash 时退化为 (size, mtime) 比较。
/// 不存在状态（size < 0）与任何存在状态都不相同。
bool sameState(const FileState &a, const FileState &b);

/// 三方差集判定。纯函数，无副作用，无 I/O —— 便于单元测试。
SyncAction decide(const FileState &L, const FileState &S, const FileState &R);

/// 生成冲突副本的文件名（不含目录）。
/// 例：报告.docx + 设备名"笔记本" + 时间 2026-09-24 15:30:00
///     → "报告 (冲突副本 来自笔记本 20260924-153000).docx"
QString makeConflictName(const QString &originalName,
                         const QString &peerDeviceName,
                         qint64 detectedAtEpoch);

} // namespace Sync
