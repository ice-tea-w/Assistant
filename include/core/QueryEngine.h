#pragma once

#include <QList>
#include <QString>

#include "core/FileRules.h"
#include "core/ScoreEngine.h"

class SnapshotManager;

/// 查询引擎（架构文档 §4.1）：串起「解析 → 候选 → 打分 → top-K」四步。
///
/// **只依赖快照的只读接口**（`FileSnapshot::size() / lowerAt()`），不摸容器、不碰 SQL、
/// 不认识 QML —— 这是 §3.1 的硬约束，也是 A4 能把查询逻辑从门面里搬出来的前提。
///
/// A 阶段只做「解析 → 打分 → top-K」；Phase B 在此基础上加了：
///  - 查询语法 token（`:kind` / `ext:` / `in:` / `size:` / `date:` / 短语）；
///  - 检索白名单（`KindFilter`，查询期过滤，切换不重建快照）；
///  - 只带过滤 token 时按 mtime 列“最近文件”。
///
/// 仍然**不做**（gated 优化，等 baseline 说话）：位图预筛（§4.2）、增量细化缓存（§4.5）、
/// 多词**路径**匹配（当前只匹配文件名）。
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
    ///
    /// @param kindFilter 检索白名单（L3.5）。**在逐行循环里判**，所以切换白名单
    ///        不需要重建快照、更不需要扫盘（Phase B 验收：<50 ms）。
    QList<Match> query(const QString &raw, int limit,
                       const KindFilter &kindFilter = KindFilter()) const;

private:
    const SnapshotManager &m_snapshots;
};
