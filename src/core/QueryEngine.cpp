#include "core/QueryEngine.h"

#include "core/FileSnapshot.h"
#include "core/QueryParser.h"
#include "core/SnapshotManager.h"

#include <QStringView>
#include <QVarLengthArray>

#include <algorithm>
#include <queue>
#include <vector>

namespace {

struct Candidate
{
    int               index = -1;
    int               score = 0;
    ScoreEngine::Tier tier  = ScoreEngine::Tier::None;
};

/// 排序规则：分数降序，同分按文件名（已折叠小写）升序。
/// 与旧 `FileSearcher::search()` 的 tie-break 一致，但不再分配字符串。
struct Rank
{
    const FileSnapshot *snapshot = nullptr;

    bool better(const Candidate &a, const Candidate &b) const
    {
        if (a.score != b.score)
            return a.score > b.score;
        return snapshot->lowerAt(a.index) < snapshot->lowerAt(b.index);
    }

    /// `std::priority_queue` 的 top() 是"最大"元素。这里把"更**差**"定义为"更大"，
    /// 于是 top() 恰好是当前 K 个里最差的那个，来新候选就淘汰它（O(log K)）。
    bool operator()(const Candidate &a, const Candidate &b) const
    {
        return better(a, b);   // "更好" == 更小
    }
};

} // namespace

QueryEngine::QueryEngine(const SnapshotManager &snapshots)
    : m_snapshots(snapshots)
{
}

QList<QueryEngine::Match> QueryEngine::query(const QString &raw, int limit) const
{
    QList<Match> out;
    if (limit <= 0)
        return out;

    const std::shared_ptr<const FileSnapshot> snapshot = m_snapshots.current();
    if (!snapshot || snapshot->empty())
        return out;

    const QueryParser::Parsed parsed = QueryParser::parse(raw);
    if (parsed.terms.isEmpty())
        return out;   // 空查询 / 纯否定项：文件不进结果（与旧语义一致）

    QVarLengthArray<QStringView, 4> terms;
    terms.reserve(static_cast<int>(parsed.terms.size()));
    for (const QString &t : parsed.terms)
        terms.append(QStringView(t));

    QVarLengthArray<QStringView, 2> negations;
    for (const QString &t : parsed.negations)
        negations.append(QStringView(t));

    const Rank rank{ snapshot.get() };
    std::priority_queue<Candidate, std::vector<Candidate>, Rank> heap(rank);

    const int count     = snapshot->size();
    const int termCount = static_cast<int>(terms.size());

    for (int i = 0; i < count; ++i) {
        // QStringView 是零拷贝视图；快照在整个查询期间由上面的 shared_ptr 保活
        const QStringView text(snapshot->lowerAt(i));

        int               sum     = 0;
        ScoreEngine::Tier best    = ScoreEngine::Tier::None;
        bool              matched = true;

        for (const QStringView &term : terms) {
            const ScoreEngine::Match m = ScoreEngine::score(term, text);
            if (!m.matched()) {
                matched = false;
                break;
            }
            sum += m.score;
            if (m.tier > best)
                best = m.tier;
        }
        if (!matched)
            continue;

        bool excluded = false;
        for (const QStringView &neg : negations) {
            if (ScoreEngine::contains(neg, text)) {
                excluded = true;
                break;
            }
        }
        if (excluded)
            continue;

        // 多词取**平均分**：既保持与单档同一尺度（不会因为词多就冲到 2000），
        // 又比"取最弱词"更能反映整体匹配质量。
        const Candidate candidate{ i, sum / termCount, best };

        if (static_cast<int>(heap.size()) < limit) {
            heap.push(candidate);
        } else if (rank.better(candidate, heap.top())) {
            heap.pop();
            heap.push(candidate);
        }
    }

    std::vector<Candidate> picked;
    picked.reserve(heap.size());
    while (!heap.empty()) {
        picked.push_back(heap.top());
        heap.pop();
    }
    std::sort(picked.begin(), picked.end(),
              [&rank](const Candidate &a, const Candidate &b) { return rank.better(a, b); });

    out.reserve(static_cast<int>(picked.size()));
    for (const Candidate &c : picked)
        out.append({ c.index, c.score, c.tier });
    return out;
}
