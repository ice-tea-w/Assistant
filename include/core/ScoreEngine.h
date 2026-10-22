#pragma once

#include <QStringView>

/// 打分内核（架构文档 §4.1 的第 4–5 步：tier 判定 + 精细打分）。
///
/// 设计要点（A4）：
///  - **零分配**：输入是 `QStringView`（快照里已经预折叠小写），内部只用栈上定长数组。
///    旧 `FuzzyMatcher::score()` 每次调用要做 2 次 `toLower()` + 每次词边界判定一次
///    `t.mid(i)` —— 2 万条一遍就是 333 ms，这是 A4 要消灭的大头。
///  - **tier 语义与旧 `FuzzyMatcher` 等价**：精确 1000 / 前缀 900 / 词边界 800 /
///    包含 600−2·pos（下限 400）/ 编辑距离 400−50·dist。**这四个数值一个都没改**，
///    因为文件名分数要和应用/内建项的分数放在同一个尺度上比较（`AppBridge::search`
///    按 score 统一排序），改尺度会让跨模块排序漂移。
///  - **新增子序列匹配**（修 P2-2）：分数落在 (350, 400) 这个**原本空着**的区间里，
///    所以既不去挤动旧档位，也不会让"散着命中"压过"连续命中"。
namespace ScoreEngine {

/// 档位强度递增（用于"多词查询取最强档"与诊断）
enum class Tier
{
    None = 0,
    EditDistance,
    Subsequence,
    Contains,
    WordBoundary,
    Prefix,
    Exact,
};

struct Match
{
    int  score = 0;
    Tier tier  = Tier::None;

    bool matched() const { return tier != Tier::None; }
};

/// 对外承诺的分数档位。**改这里会让跨模块排序漂移，先想清楚**。
namespace Band {
constexpr int kExact          = 1000;
constexpr int kPrefix         = 900;
constexpr int kWordBoundary   = 800;
constexpr int kContainsMax    = 600;   ///< 600 − 2·pos，最低 400
constexpr int kContainsFloor  = 400;
constexpr int kSubsequenceMax = 399;   ///< 子序列独占 (350, 400)
constexpr int kSubsequenceMin = 351;
constexpr int kEditBase       = 400;   ///< 旧公式 400 − 50·dist；dist ≥ 1 所以实际 ≤ 350
constexpr int kEditStep       = 50;
} // namespace Band

/// 对一条已折叠小写的文件名打分。`query` 必须是单个词（多词由 QueryEngine 拆开）。
Match score(QStringView queryLower, QStringView textLower);

/// 廉价子串判定（否定项 `-词` 用；不需要档位与排序分数）。
bool contains(QStringView queryLower, QStringView textLower);

} // namespace ScoreEngine
