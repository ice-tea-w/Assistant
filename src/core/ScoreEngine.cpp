#include "core/ScoreEngine.h"

#include <algorithm>
#include <array>

namespace ScoreEngine {

namespace {

/// 旧 `FuzzyMatcher` 的编辑距离档只对短串启用，这里沿用同一组上限
constexpr int kMaxEditQueryLen = 10;
constexpr int kMaxEditTextLen  = 30;

/// tier-3（词边界）用的分隔符：与旧实现**逐字节一致**（只有这 4 个）。
/// 不要在这里加 '/' '\\' '(' —— 那会改变旧档位的判定结果。
bool isTierWordSeparator(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('-')
        || c == QLatin1Char('_') || c == QLatin1Char('.');
}

/// 子序列**加分**用的更宽的分隔符集（只影响新档位的内部排序）
bool isBonusBoundary(QChar c)
{
    return isTierWordSeparator(c) || c == QLatin1Char('/')
        || c == QLatin1Char('\\') || c == QLatin1Char('(');
}

bool startsWithAt(QStringView text, QStringView needle, qsizetype at)
{
    if (at < 0 || at + needle.size() > text.size())
        return false;
    for (qsizetype i = 0; i < needle.size(); ++i) {
        if (text.at(at + i) != needle.at(i))
            return false;
    }
    return true;
}

qsizetype indexOfSub(QStringView text, QStringView needle)
{
    if (needle.isEmpty())
        return 0;
    if (needle.size() > text.size())
        return -1;

    const qsizetype last = text.size() - needle.size();
    for (qsizetype i = 0; i <= last; ++i) {
        if (text.at(i) != needle.at(0))
            continue;
        if (startsWithAt(text, needle, i))
            return i;
    }
    return -1;
}

/// 带守卫的 Levenshtein（栈上两行，零分配）。
/// 调用前必须已保证 `a.size() <= 10 && b.size() <= 30`。
int editDistanceBounded(QStringView a, QStringView b)
{
    const int len1 = static_cast<int>(a.size());
    const int len2 = static_cast<int>(b.size());

    std::array<int, kMaxEditTextLen + 2> prev{};
    std::array<int, kMaxEditTextLen + 2> curr{};

    for (int j = 0; j <= len2; ++j)
        prev[j] = j;

    for (int i = 0; i < len1; ++i) {
        curr[0] = i + 1;
        const QChar ca = a.at(i);
        for (int j = 0; j < len2; ++j) {
            const int cost = (ca == b.at(j)) ? 0 : 1;
            curr[j + 1] = std::min({ prev[j + 1] + 1,    // 删除
                                     curr[j] + 1,        // 插入
                                     prev[j] + cost });  // 替换
        }
        std::swap(prev, curr);
    }
    return prev[len2];
}

/// 子序列匹配（**贪心最左**，O(m) 而不是 O(n·m)）。
///
/// 为什么不是 §4.3 里的 fzf v2 Smith-Waterman DP：baseline 说话 ——
/// DP 让 4–8 字符档比旧实现**慢 20%**（`docs/devlog/2026-10-22.md` 的第二轮数据），
/// 因为 DP 失败后还要再跑一遍编辑距离，等于两倍 DP 工作量。
/// 贪心最左对"是不是子序列"的判定与 DP **完全等价**（子序列存在性用贪心最优），
/// 只是不保证找到**分数最高**的那种对齐；在 (350,400) 这个窄档里够用。
/// fzf 式 DP 留作 gated 候选：等 §4.2 位图预筛把 DP 调用次数压下来后再上。
int subsequenceRawScore(QStringView query, QStringView text)
{
    const int n = static_cast<int>(query.size());
    const int m = static_cast<int>(text.size());
    if (n == 0 || n > m)
        return 0;

    constexpr int kScoreMatch       = 16;
    constexpr int kBonusBoundary    = 8;
    constexpr int kBonusConsecutive = 4;

    int score    = 0;
    int cursor   = 0;      // 文本里的扫描位置（只前进，所以整体 O(m)）
    int previous = -2;     // 上一个命中的下标（用于"连续命中"加分）

    for (int i = 0; i < n; ++i) {
        const QChar qc = query.at(i);

        int j = cursor;
        while (j < m && text.at(j) != qc)
            ++j;
        if (j >= m)
            return 0;      // 不是子序列

        score += kScoreMatch;
        if (j == 0 || isBonusBoundary(text.at(j - 1)))
            score += kBonusBoundary;

        if (j == previous + 1) {
            score += kBonusConsecutive;            // 紧跟上一次命中
        } else if (i > 0) {
            score -= 1 + (j - cursor);             // 跳字惩罚（gap 起始 + 跳过的字符数）
        }

        previous = j;
        cursor   = j + 1;
    }

    return score > 0 ? score : 0;
}

/// 编辑距离档。条件与公式与旧 `FuzzyMatcher` 完全一致（含"0 分等于不匹配"这一点）。
Match editDistanceMatch(QStringView query, QStringView text)
{
    if (query.size() > kMaxEditQueryLen || text.size() > kMaxEditTextLen)
        return {};

    const int dist   = editDistanceBounded(query, text);
    const int maxLen = std::max(static_cast<int>(query.size()), static_cast<int>(text.size()));
    if (dist > maxLen / 2)
        return {};

    const int score = std::max(0, Band::kEditBase - dist * Band::kEditStep);
    if (score <= 0)          // 旧实现返回 0，调用方按 s > 0 过滤 —— 等价于"不匹配"
        return {};

    return { score, Tier::EditDistance };
}

} // namespace

Match score(QStringView queryLower, QStringView textLower)
{
    if (queryLower.isEmpty() || textLower.isEmpty())
        return {};

    // 1. 精确
    if (queryLower.size() == textLower.size() && startsWithAt(textLower, queryLower, 0))
        return { Band::kExact, Tier::Exact };

    // 2. 前缀
    if (startsWithAt(textLower, queryLower, 0))
        return { Band::kPrefix, Tier::Prefix };

    // 3. 词边界（位置 i 的前一个字符是分隔符）
    for (qsizetype i = 1; i + queryLower.size() <= textLower.size(); ++i) {
        if (isTierWordSeparator(textLower.at(i - 1)) && startsWithAt(textLower, queryLower, i))
            return { Band::kWordBoundary, Tier::WordBoundary };
    }

    // 4. 包含（位置加权，公式与旧实现逐位一致）
    if (const qsizetype pos = indexOfSub(textLower, queryLower); pos >= 0) {
        const int penalty = static_cast<int>(std::min<qsizetype>(pos * 2, 200));
        return { Band::kContainsMax - penalty, Tier::Contains };
    }

    // 5. 子序列（新增）。分数严格落在 (350, 400)：既不挤动旧档位，
    //    也保证"散着命中"排在"连续命中"之后、"拼错"之前。
    if (const int raw = subsequenceRawScore(queryLower, textLower); raw > 0) {
        const int bonus = std::min(raw / 2, Band::kSubsequenceMax - Band::kSubsequenceMin);
        return { Band::kSubsequenceMin + bonus, Tier::Subsequence };
    }

    // 6. 拼写容错（最弱档）
    return editDistanceMatch(queryLower, textLower);
}

bool contains(QStringView queryLower, QStringView textLower)
{
    return !queryLower.isEmpty() && indexOfSub(textLower, queryLower) >= 0;
}

} // namespace ScoreEngine
