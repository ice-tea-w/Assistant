#include "core/QueryEngine.h"

#include "core/FileFlags.h"
#include "core/FileKind.h"
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

/// 只带过滤 token（`:doc`）时的排序：mtime 降序，同刻按文件名升序。
struct RecentRank
{
    const FileSnapshot *snapshot = nullptr;

    bool better(int a, int b) const
    {
        const qint64 ma = snapshot->mtimeAt(a);
        const qint64 mb = snapshot->mtimeAt(b);
        if (ma != mb)
            return ma > mb;
        return snapshot->lowerAt(a) < snapshot->lowerAt(b);
    }

    bool operator()(int a, int b) const { return better(a, b); }
};

} // namespace

QueryEngine::QueryEngine(const SnapshotManager &snapshots)
    : m_snapshots(snapshots)
{
}

QList<QueryEngine::Match> QueryEngine::query(const QString &raw, int limit,
                                             const KindFilter &kindFilter) const
{
    QList<Match> out;
    if (limit <= 0)
        return out;

    const std::shared_ptr<const FileSnapshot> snapshot = m_snapshots.current();
    if (!snapshot || snapshot->empty())
        return out;

    const QueryParser::Parsed parsed = QueryParser::parse(raw);
    if (parsed.isEmpty())
        return out;   // 空查询 / 纯否定项：文件不进结果（与旧语义一致）

    QVarLengthArray<QStringView, 4> terms;
    terms.reserve(static_cast<int>(parsed.terms.size()));
    for (const QString &t : parsed.terms)
        terms.append(QStringView(t));

    QVarLengthArray<QStringView, 2> negations;
    for (const QString &t : parsed.negations)
        negations.append(QStringView(t));

    // `in:` 的两种形态预先准备好，避免在逐行循环里做字符串拼接
    QVarLengthArray<QString, 2>     inWrapped;   // "/token/" 的持有者（QStringView 不能悬空）
    inWrapped.reserve(static_cast<int>(parsed.inFilters.size()));
    for (const QString &f : parsed.inFilters)
        inWrapped.append(QStringLiteral("/") + f + QStringLiteral("/"));

    QVarLengthArray<QStringView, 2> inTokens;
    QVarLengthArray<QStringView, 2> inSegments;
    inTokens.reserve(inWrapped.size());
    inSegments.reserve(inWrapped.size());
    for (int k = 0; k < parsed.inFilters.size(); ++k) {
        inTokens.append(QStringView(parsed.inFilters.at(k)));
        inSegments.append(QStringView(inWrapped.at(k)));
    }

    QVarLengthArray<QStringView, 4> extensionViews;
    extensionViews.reserve(static_cast<int>(parsed.extensions.size()));
    for (const QString &e : parsed.extensions)
        extensionViews.append(QStringView(e));

    const int count = snapshot->size();

    // 过滤谓词先判（一条 `&` / 一次比较），避免为将被过滤掉的条目做打分。
    auto passesFilters = [&](int i) -> bool {
        uint16_t mask = snapshot->catMaskAt(i);
        if (mask == 0)
            mask = FileKind::Other;   // 迁移前的旧行：按“未归类”对待

        // L3.5 检索白名单：**查询期**判（切换白名单不重建快照、不扫盘）
        if (!kindFilter.acceptsLower(mask, QStringView(snapshot->lowerAt(i))))
            return false;

        if (parsed.hasKind && (mask & parsed.kindMask) == 0)
            return false;

        if (!extensionViews.isEmpty()) {
            const QStringView name = snapshot->lowerAt(i);
            const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
            if (dot <= 0)
                return false;
            const QStringView ext = name.mid(dot + 1);
            bool hit = false;
            for (const QStringView &wanted : extensionViews) {
                if (ext == wanted) {
                    hit = true;
                    break;
                }
            }
            if (!hit)
                return false;
        }

        if (!inTokens.isEmpty()) {
            const QStringView path = snapshot->pathViewAt(i);
            bool hit = false;
            for (int k = 0; k < inTokens.size(); ++k) {
                // 路径前缀（`in:C:/work`）或任意一段目录名（`in:downloads`）
                if (path.startsWith(inTokens.at(k), Qt::CaseInsensitive)
                    || path.contains(inSegments.at(k), Qt::CaseInsensitive)) {
                    hit = true;
                    break;
                }
            }
            if (!hit)
                return false;
        }

        if (parsed.hasSize) {
            const qint64 size = snapshot->sizeAt(i);
            if (parsed.sizeMin >= 0 && size < parsed.sizeMin)
                return false;
            if (parsed.sizeMax >= 0 && size > parsed.sizeMax)
                return false;
        }

        if (parsed.hasDate) {
            const qint64 mtime = snapshot->mtimeAt(i);
            if (parsed.dateAfter > 0 && mtime < parsed.dateAfter)
                return false;
            if (parsed.dateBefore > 0 && mtime >= parsed.dateBefore)
                return false;
        }

        return true;
    };

    auto passesNegations = [&](int i) -> bool {
        const QStringView text(snapshot->lowerAt(i));
        for (const QStringView &neg : negations) {
            if (ScoreEngine::contains(neg, text))
                return false;
        }
        return true;
    };

    // ---- 只带过滤 token（无自由词）：按 mtime 列“最近文件” ----
    if (terms.isEmpty()) {
        if (!parsed.hasFilters())
            return out;   // 只有否定项：与旧语义一致，不出文件

        const RecentRank recent{ snapshot.get() };
        std::priority_queue<int, std::vector<int>, RecentRank> heap(recent);

        for (int i = 0; i < count; ++i) {
            if (!passesFilters(i) || !passesNegations(i))
                continue;
            if (static_cast<int>(heap.size()) < limit) {
                heap.push(i);
            } else if (recent.better(i, heap.top())) {
                heap.pop();
                heap.push(i);
            }
        }

        std::vector<int> picked;
        picked.reserve(heap.size());
        while (!heap.empty()) {
            picked.push_back(heap.top());
            heap.pop();
        }
        std::sort(picked.begin(), picked.end(),
                  [&recent](int a, int b) { return recent.better(a, b); });

        out.reserve(static_cast<int>(picked.size()));
        for (int index : picked)
            out.append({ index, 100, ScoreEngine::Tier::Recent });
        return out;
    }

    // ---- 自由词查询：过滤 → 打分 → top-K ----
    const Rank rank{ snapshot.get() };
    std::priority_queue<Candidate, std::vector<Candidate>, Rank> heap(rank);

    const int termCount = static_cast<int>(terms.size());

    for (int i = 0; i < count; ++i) {
        if (!passesFilters(i) || !passesNegations(i))
            continue;

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

        // 多词取**平均分**：既保持与单档同一尺度（不会因为词多就冲到 2000），
        // 又比"取最弱词"更能反映整体匹配质量。
        int score = sum / termCount;

        // demote 策略：工程目录内的文件仍然可搜，但降权 30%（治理文档 §1.2）
        if ((snapshot->flagsAt(i) & FileFlags::Project) != 0)
            score = score * 7 / 10;

        const Candidate candidate{ i, score, best };

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
