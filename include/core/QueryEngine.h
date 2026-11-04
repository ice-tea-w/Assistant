#pragma once

#include <QList>
#include <QString>

#include "core/ScoreEngine.h"

class SnapshotManager;

/// 查询引擎（架构文档 §4.1）：串起「解析 → 候选 → 打分 → top-K」四步。
///
/// **只依赖快照的只读接口**（`FileSnapshot::size() / lowerAt()`），不摸容器、不碰 SQL、
/// 不认识 QML —— 这是 §3.1 的硬约束，也是 A4 能把查询逻辑从门面里搬出来的前提。
///
/// A 阶段刻意**不做**（都是 gated 优化，等 baseline 说话）：
///  - 位图预筛（§4.2）、增量细化缓存（§4.5）；
///  - 查询语法 token（`ext:` / `in:` / `size:` …）—— Phase B 的 QueryParser 扩展；
///  - 多词**路径**匹配 —— A 阶段只匹配文件名。
class QueryEngine
{
public:
    struct Match
    {
        int               index = -1;   ///< 快照下标
        int               score = 0;
        ScoreEngine::Tier tier  = ScoreEngine::Tier::None;
    };

    explicit QueryEngine(const SnapshotManager &snapshots);

    /// 解析原始输入 → 逐条打分 → top-K（min-heap，只分配 K 个结果，替代全排序）。
    /// 空查询 / 只有否定项 / 快照未就绪 → 空列表。
    QList<Match> query(const QString &raw, int limit) const;

private:
    const SnapshotManager &m_snapshots;
};
