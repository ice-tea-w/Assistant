/// A4 验收工具：**语义 golden** + **与旧引擎的不回归对比**。
///
/// A2 时期这个工具断言的"逐条一致"在 A4 已经不成立了 —— 新引擎**故意**多出
/// 子序列匹配（`chr` → `chrome_installer.exe`）。所以断言换成三条**更强也更准确**的性质：
///
/// 1. **超集**：旧引擎能匹配的，新引擎必须也能匹配（子序列只会新增，不会丢）；
/// 2. **分数不回归**：旧分数 ≤ 新分数。而且旧实现里"有意义"的档位
///    （>350，即精确/前缀/词边界/包含）新引擎必须**给同一个分数**——因为那四档
///    的数值是跨模块共用的（`AppBridge` 里文件名和应用/内建项同尺度排序）；
/// 3. **top-K 顺序不回归**：旧 top-20 里分数 >350 的条目，必须仍在新的 top-20 里，
///    且相对顺序不变（只有旧分数 ≤350 的弱命中允许被更强的子序列命中挤掉）。
///
/// 外加 [phase 3] 的绝对语义用例：子序列 / 拼写容错 / 中文 / 多词 AND / 否定项 /
/// 空查询 / **路径不参与匹配**。
///
/// ```
///   assistant_golden                       # 合成语料 20 万条（A2 定的规模）
///   assistant_golden --n=20000 --real      # 加跑真机数据（v2 压到 20000 与旧引擎同候选集）
/// ```

#include "core/FileIndex.h"
#include "core/FileSnapshot.h"
#include "core/FileSearcher.h"
#include "core/FuzzyMatcher.h"
#include "core/QueryEngine.h"
#include "core/QueryParser.h"
#include "core/ScoreEngine.h"
#include "core/SnapshotManager.h"
#include "ui/SearchResult.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileInfoList>
#include <QHash>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_checks = 0;
int g_fails  = 0;

void report(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  FAIL  %s\n", what.toUtf8().constData());
    }
}

/// 真机等价比对用的上限：与旧 `FileSearcher` 的 `kMaxIndexedFiles` 相同
constexpr int kComparableLimit = 20000;

/// 旧实现里"有意义"的档位下限：>350 只可能来自精确(1000)/前缀(900)/词边界(800)/包含(400..600)。
/// 编辑距离档最高 350，子序列档最低 351 —— 所以 350 是条干净的分界线。
constexpr int kMeaningfulScore = 350;

// ---------------------------------------------------------------------------
// 旧实现（逐行转写自 src/core/FileSearcher.cpp 的 search()）
// ---------------------------------------------------------------------------

struct LegacyHit
{
    QString name;
    QString path;
    int     score = 0;
};

QList<LegacyHit> legacySearchSnapshot(const FileSnapshot &snap, const QString &query, int limit)
{
    QList<LegacyHit> matches;
    if (query.trimmed().isEmpty() || limit <= 0)
        return matches;

    matches.reserve(1024);
    for (int i = 0; i < snap.size(); ++i) {
        const int s = FuzzyMatcher::score(query, snap.nameAt(i));
        if (s > 0)
            matches.append({ snap.nameAt(i), snap.pathAt(i), s });
    }

    std::sort(matches.begin(), matches.end(), [](const LegacyHit &a, const LegacyHit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    if (matches.size() > limit)
        matches.erase(matches.begin() + limit, matches.end());
    return matches;
}

// ---------------------------------------------------------------------------
// 通用回归检查
// ---------------------------------------------------------------------------

/// 超集 + 分数不回归
void checkSupersetAndScores(const QList<LegacyHit> &oldHits,
                            const QList<SearchResult> &newHits,
                            const QString &tag)
{
    QHash<QString, int> newScoreByName;
    newScoreByName.reserve(newHits.size());
    for (const SearchResult &r : newHits)
        newScoreByName.insert(r.title, r.score);

    int missing = 0;
    int regressed = 0;
    QString firstProblem;

    for (const LegacyHit &old : oldHits) {
        const auto it = newScoreByName.constFind(old.name);
        if (it == newScoreByName.constEnd()) {
            ++missing;
            if (firstProblem.isEmpty())
                firstProblem = QStringLiteral("旧命中 '%1'（%2 分）新引擎找不到")
                                   .arg(old.name).arg(old.score);
            continue;
        }

        if (old.score > kMeaningfulScore) {
            // 四个强档位必须**逐位相同**（跨模块排序依赖这个尺度）
            if (it.value() != old.score) {
                ++regressed;
                if (firstProblem.isEmpty())
                    firstProblem = QStringLiteral("'%1' 分数变了：旧 %2 → 新 %3")
                                       .arg(old.name).arg(old.score).arg(it.value());
            }
        } else if (it.value() < old.score) {
            ++regressed;
            if (firstProblem.isEmpty())
                firstProblem = QStringLiteral("'%1' 分数回归：旧 %2 → 新 %3")
                                   .arg(old.name).arg(old.score).arg(it.value());
        }
    }

    report(missing == 0, QStringLiteral("%1 超集（缺 %2 条%3）")
                             .arg(tag).arg(missing)
                             .arg(firstProblem.isEmpty() ? QString()
                                                         : QStringLiteral("；") + firstProblem));
    report(regressed == 0, QStringLiteral("%1 分数不回归（%2 条退化）").arg(tag).arg(regressed));
}

/// top-K 顺序不回归：旧 top-20 中分数 >350 的条目，新 top-20 必须包含且保持相对顺序。
void checkTopKOrder(const QList<LegacyHit> &oldTop, const QList<SearchResult> &newTop,
                    const QString &tag)
{
    QStringList newOrder;
    for (const SearchResult &r : newTop)
        newOrder << r.title;

    QStringList oldMeaningful;
    for (const LegacyHit &h : oldTop) {
        if (h.score > kMeaningfulScore)
            oldMeaningful << h.name;
    }

    bool ok = true;
    QString why;
    int cursor = 0;
    for (const QString &name : oldMeaningful) {
        const int at = static_cast<int>(newOrder.indexOf(name));
        if (at < 0) {
            ok = false;
            why = QStringLiteral("'%1' 掉出新 top-20").arg(name);
            break;
        }
        if (at < cursor) {
            ok = false;
            why = QStringLiteral("'%1' 顺序前移 %2 → %3").arg(name).arg(cursor).arg(at);
            break;
        }
        cursor = at;
    }

    report(ok, QStringLiteral("%1 top-20 顺序不回归%2")
                   .arg(tag)
                   .arg(ok ? QString() : QStringLiteral("：") + why));
}

// ---------------------------------------------------------------------------
// 语料
// ---------------------------------------------------------------------------

std::vector<QString> makeCorpus(int n, unsigned seed)
{
    static const char *kWords[] = {
        "report", "invoice", "budget", "chrome", "installer", "photo", "meeting",
        "notes", "design", "draft", "final", "backup", "archive", "screenshot",
        "presentation", "quarterly", "roadmap", "dataset", "config", "readme",
    };
    static const char *kExts[] = {
        "docx", "pdf", "xlsx", "png", "cpp", "h", "txt", "md", "mp4", "zip",
    };
    static const char *kCjk[] = { "报告", "会议纪要", "预算表", "设计稿", "截图" };

    std::mt19937 rng(seed);
    std::vector<QString> out;
    out.reserve(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        QString name;
        const int parts = 1 + static_cast<int>(rng() % 3);
        for (int p = 0; p < parts; ++p) {
            if (rng() % 10 == 0)
                name += QString::fromUtf8(kCjk[rng() % 5]);
            else
                name += QString::fromLatin1(kWords[rng() % 20]);
            if (p + 1 < parts)
                name += QLatin1Char('_');
        }
        name += QLatin1Char('_') + QString::number(i);
        name += QLatin1Char('.') + QString::fromLatin1(kExts[rng() % 10]);
        out.push_back(name);
    }
    return out;
}

QStringList defaultQueries(const std::vector<QString> &names)
{
    QStringList queries = {
        QStringLiteral("report"), QStringLiteral("rep"), QStringLiteral("port"),
        QStringLiteral("chr"), QStringLiteral("report_1"),
        QString::fromUtf8("报告"), QString::fromUtf8("预算"),
        QStringLiteral("a"), QStringLiteral("z"),
        QStringLiteral("xyzzy_no_such_file"),
        QString(), QStringLiteral("   "),
    };
    if (!names.empty()) {
        queries << names.front();
        queries << names.back();
        queries << names.front().left(6);
    }
    return queries;
}

// ---------------------------------------------------------------------------
// 阶段①：合成语料
// ---------------------------------------------------------------------------

void runSynthetic(int n)
{
    std::printf("\n[phase 1] 合成语料 N=%d（超集 + 分数 + top-20 顺序）\n", n);

    const std::vector<QString> names = makeCorpus(n, 20261022u);

    FileSnapshot::Builder builder;
    builder.reserve(n);
    for (const QString &name : names)
        builder.add(name, QStringLiteral("C:/corpus/") + name, 1024, 1700000000);

    std::shared_ptr<FileSnapshot> snap = builder.finish();
    FileIndex index;
    index.setSnapshot(snap);

    const QStringList queries = defaultQueries(names);

    QElapsedTimer timer;
    timer.start();

    for (const QString &q : queries) {
        // limit 给足：验证"旧引擎能命中的一条都不丢"
        const QList<LegacyHit> oldAll = legacySearchSnapshot(*snap, q, n + 1);
        const QList<SearchResult> newAll = index.query(q, n + 1);
        checkSupersetAndScores(oldAll, newAll, QStringLiteral("q='%1'").arg(q));

        const QList<LegacyHit> oldTop = legacySearchSnapshot(*snap, q, 20);
        const QList<SearchResult> newTop = index.query(q, 20);
        checkTopKOrder(oldTop, newTop, QStringLiteral("q='%1'").arg(q));
    }

    std::printf("  %d 条查询 × 2 项性质：%lld ms\n", static_cast<int>(queries.size()),
                static_cast<long long>(timer.elapsed()));
}

// ---------------------------------------------------------------------------
// 阶段②：真机数据
// ---------------------------------------------------------------------------

QStringList harvestQueries(int n)
{
    QStringList out;
    const QString desktop =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty())
        return out;

    // 自己控制递归：QDirIterator::Subdirectories 会跟着 junction 钻下去（A1 的 P0-1）
    QStringList stack{ desktop };
    while (!stack.isEmpty() && out.size() < n) {
        const QString dir = stack.takeLast();
        const QFileInfoList entries =
            QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
        for (const QFileInfo &fi : entries) {
            if (out.size() >= n)
                break;
            if (fi.isDir()) {
                if (fi.isSymLink() || fi.isJunction())
                    continue;
                stack.append(fi.filePath());
                continue;
            }
            if (fi.isSymLink())
                continue;
            const QString name = fi.fileName();
            if (name.length() < 3)
                continue;
            out << name;
            out << name.left(qMax(3, name.length() / 2));
        }
    }
    return out;
}

void runReal(int sampleQueries)
{
    std::printf("\n[phase 2] 真机数据（v2 limit=%d vs 旧 FileSearcher）\n", kComparableLimit);

    QElapsedTimer legacyTimer;
    legacyTimer.start();
    const int legacyCount = FileSearcher::buildIndex();
    const qint64 legacyMs = legacyTimer.elapsed();

    FileIndex index;
    index.setEntryLimit(kComparableLimit);

    QEventLoop loop;
    QObject::connect(&index, &FileIndex::indexReady, &loop, &QEventLoop::quit);

    QElapsedTimer v2Timer;
    v2Timer.start();
    index.startAsync();
    loop.exec();
    const qint64 v2Ms = v2Timer.elapsed();

    const FileIndex::IndexStats st = index.stats();
    std::printf("  legacy: %d 条 / %lld ms    v2: %d 条 / %lld ms（扫描 %lld ms）\n",
                legacyCount, static_cast<long long>(legacyMs),
                st.entries, static_cast<long long>(v2Ms),
                static_cast<long long>(st.msLastScan));

    report(st.entries == legacyCount,
           QStringLiteral("候选集规模一致（legacy %1 vs v2 %2）")
               .arg(legacyCount).arg(st.entries));

    const QStringList queries = harvestQueries(sampleQueries);
    std::printf("  用 %d 条真实查询比对\n", static_cast<int>(queries.size()));

    for (const QString &q : queries) {
        const QList<FileSearcher::Entry> oldHits = FileSearcher::search(q, 200);
        const QList<SearchResult> newHits = index.query(q, 200);

        QStringList newTitles;
        newTitles.reserve(newHits.size());
        QHash<QString, int> newScoreByName;
        for (const SearchResult &r : newHits) {
            newTitles << r.title;
            newScoreByName.insert(r.title, r.score);
        }

        bool ok = true;
        QString why;
        for (const FileSearcher::Entry &old : oldHits) {
            const auto it = newScoreByName.constFind(old.name);
            if (it == newScoreByName.constEnd()) {
                ok = false;
                why = QStringLiteral("'%1' 缺失").arg(old.name);
                break;
            }
            if (old.score > kMeaningfulScore && it.value() != old.score) {
                ok = false;
                why = QStringLiteral("'%1' 分数变了 %2 → %3")
                          .arg(old.name).arg(old.score).arg(it.value());
                break;
            }
            if (old.score <= kMeaningfulScore && it.value() < old.score) {
                ok = false;
                why = QStringLiteral("'%1' 分数回归 %2 → %3")
                          .arg(old.name).arg(old.score).arg(it.value());
                break;
            }
        }
        report(ok, QStringLiteral("q='%1'（旧 %2 条）%3")
                       .arg(q).arg(oldHits.size())
                       .arg(ok ? QString() : QStringLiteral("：") + why));
    }

    std::printf("  真机比对 %d 条查询完成\n", static_cast<int>(queries.size()));

    if (!queries.isEmpty()) {
        QElapsedTimer t1;
        t1.start();
        (void)FileSearcher::search(queries.first(), 20);
        const qint64 oldUs = t1.nsecsElapsed() / 1000;

        QElapsedTimer t2;
        t2.start();
        (void)index.query(queries.first(), 20);
        const qint64 newUs = t2.nsecsElapsed() / 1000;

        std::printf("  单条查询（'%s'）：旧 %.1f ms → 新 %.1f ms（%.1f×）\n",
                    queries.first().toUtf8().constData(),
                    oldUs / 1000.0, newUs / 1000.0,
                    newUs > 0 ? static_cast<double>(oldUs) / static_cast<double>(newUs) : 0.0);
    }
}

// ---------------------------------------------------------------------------
// 阶段③：绝对语义用例
// ---------------------------------------------------------------------------

void runSemanticGolden()
{
    std::printf("\n[phase 3] 语义 golden 用例（绝对断言）\n");

    struct Case
    {
        QString name;
        QString path;
    };
    const QVector<Case> files = {
        { QStringLiteral("chrome_installer.exe"), QStringLiteral("C:/t/chrome_installer.exe") },
        { QStringLiteral("Chrome Installer"),     QStringLiteral("C:/t/Chrome Installer") },
        { QStringLiteral("report_final.docx"),    QStringLiteral("C:/t/report_final.docx") },
        { QStringLiteral("report_other.txt"),     QStringLiteral("C:/t/report_other.txt") },
        { QStringLiteral("budget.xlsx"),          QStringLiteral("C:/t/budget.xlsx") },
        // 短名字：编辑距离档只在 查询≤10 字符 且 文件名≤30 字符 时启用（旧上限）
        { QStringLiteral("budget.txt"),           QStringLiteral("C:/t/budget.txt") },
        { QString::fromUtf8("报告_2026.pdf"),
          QStringLiteral("C:/t/") + QString::fromUtf8("报告_2026.pdf") },
        // 名字里没有 report，但**所在目录**叫 report —— 用来验证"路径不参与匹配"
        { QStringLiteral("notes.md"),             QStringLiteral("C:/work/report/notes.md") },
    };

    FileSnapshot::Builder builder;
    for (const Case &c : files)
        builder.add(c.name, c.path, 10, 1700000000);
    std::shared_ptr<FileSnapshot> snap = builder.finish();

    SnapshotManager snapshots;
    snapshots.publish(snap);
    const QueryEngine engine(snapshots);

    auto titles = [&snap](const QList<QueryEngine::Match> &matches) {
        QStringList out;
        for (const QueryEngine::Match &m : matches)
            out << snap->nameAt(m.index);
        return out;
    };
    auto matchOf = [&snap](const QList<QueryEngine::Match> &matches, const QString &name) {
        for (const QueryEngine::Match &m : matches) {
            if (snap->nameAt(m.index) == name)
                return m;
        }
        return QueryEngine::Match{};
    };

    // 1. 子序列
    {
        const auto r = engine.query(QStringLiteral("chri"), 20);
        const auto m = matchOf(r, QStringLiteral("chrome_installer.exe"));
        report(m.tier == ScoreEngine::Tier::Subsequence,
               QStringLiteral("子序列 'chri' 命中 chrome_installer.exe（tier=%1）")
                   .arg(static_cast<int>(m.tier)));
        report(m.score >= ScoreEngine::Band::kSubsequenceMin
                   && m.score <= ScoreEngine::Band::kSubsequenceMax,
               QStringLiteral("子序列分数落在 (350,400) 空档（实际 %1）").arg(m.score));
    }

    // 2. 大写缩写
    {
        const auto r = engine.query(QStringLiteral("ci"), 20);
        const auto m = matchOf(r, QStringLiteral("Chrome Installer"));
        report(m.tier == ScoreEngine::Tier::Subsequence,
               QStringLiteral("缩写 'ci' 命中 Chrome Installer"));
    }

    // 3. 四个旧档位数值原样
    {
        const auto exact = matchOf(engine.query(QStringLiteral("budget.xlsx"), 5),
                                   QStringLiteral("budget.xlsx"));
        report(exact.score == ScoreEngine::Band::kExact,
               QStringLiteral("精确 = 1000（实际 %1）").arg(exact.score));

        const auto prefix = matchOf(engine.query(QStringLiteral("report_fi"), 5),
                                    QStringLiteral("report_final.docx"));
        report(prefix.score == ScoreEngine::Band::kPrefix,
               QStringLiteral("前缀 = 900（实际 %1）").arg(prefix.score));

        const auto boundary = matchOf(engine.query(QStringLiteral("final"), 5),
                                      QStringLiteral("report_final.docx"));
        report(boundary.score == ScoreEngine::Band::kWordBoundary,
               QStringLiteral("词边界 = 800（实际 %1）").arg(boundary.score));

        const auto contained = matchOf(engine.query(QStringLiteral("eport_final"), 5),
                                       QStringLiteral("report_final.docx"));
        report(contained.score == ScoreEngine::Band::kContainsMax - 2,
               QStringLiteral("包含 pos=1 = 598（实际 %1）").arg(contained.score));
    }

    // 4. 拼写容错 —— 注意这里有两种 typo，性质完全不同：
    //  4a **替换型**（budget → budgat）：不是子序列，只能靠编辑距离档救回来。
    //  4b **删除型**（budget → budgt）：**恰好是子序列**（跳过 e），于是被更强的
    //     子序列档吃掉，分数 399 —— 比编辑距离档的 350 更高。这是新引擎顺带
    //     带来的收益（少打一个字符是最常见的误输入），不是 bug。
    {
        const auto substitute = matchOf(engine.query(QStringLiteral("budgat.txt"), 10),
                                        QStringLiteral("budget.txt"));
        report(substitute.tier == ScoreEngine::Tier::EditDistance,
               QStringLiteral("替换型 typo 'budgat.txt' → budget.txt（tier=%1）")
                   .arg(static_cast<int>(substitute.tier)));
        report(substitute.score == ScoreEngine::Band::kEditBase - ScoreEngine::Band::kEditStep,
               QStringLiteral("编辑距离 1 → 350（实际 %1）").arg(substitute.score));

        const auto deletion = matchOf(engine.query(QStringLiteral("budgt.txt"), 10),
                                      QStringLiteral("budget.txt"));
        report(deletion.tier == ScoreEngine::Tier::Subsequence,
               QStringLiteral("删除型 typo 'budgt.txt' 走子序列档（tier=%1，顺带修好）")
                   .arg(static_cast<int>(deletion.tier)));
        report(deletion.score >= ScoreEngine::Band::kSubsequenceMin,
               QStringLiteral("删除型 typo 分数（%1）高于编辑距离档上限 350").arg(deletion.score));
    }

    // 5. 中文
    {
        const auto m = matchOf(engine.query(QString::fromUtf8("报告"), 10),
                               QString::fromUtf8("报告_2026.pdf"));
        report(m.score == ScoreEngine::Band::kPrefix,
               QStringLiteral("中文前缀 '报告' = 900（实际 %1）").arg(m.score));
    }

    // 6. 多词 AND
    {
        const QStringList t = titles(engine.query(QStringLiteral("report final"), 20));
        report(t.contains(QStringLiteral("report_final.docx")),
               QStringLiteral("多词 AND：'report final' 命中 report_final.docx"));
        report(!t.contains(QStringLiteral("report_other.txt")),
               QStringLiteral("多词 AND：'report final' 排除 report_other.txt"));
    }

    // 7. 否定项
    {
        const QStringList t = titles(engine.query(QStringLiteral("report -final"), 20));
        report(t.contains(QStringLiteral("report_other.txt")),
               QStringLiteral("否定项：'report -final' 保留 report_other.txt"));
        report(!t.contains(QStringLiteral("report_final.docx")),
               QStringLiteral("否定项：'report -final' 排除 report_final.docx"));
    }

    // 8. 空查询 / 纯否定项
    {
        report(engine.query(QString(), 20).isEmpty(), QStringLiteral("空查询返回空"));
        report(engine.query(QStringLiteral("   "), 20).isEmpty(), QStringLiteral("空白查询返回空"));
        report(engine.query(QStringLiteral("-final"), 20).isEmpty(),
               QStringLiteral("纯否定项返回空（A 阶段语义）"));
    }

    // 9. 路径不参与匹配
    {
        report(titles(engine.query(QStringLiteral("notes"), 20)).contains(QStringLiteral("notes.md")),
               QStringLiteral("按文件名命中 notes.md"));
        report(!titles(engine.query(QStringLiteral("work"), 20)).contains(QStringLiteral("notes.md")),
               QStringLiteral("路径不参与匹配：query 'work' 不命中 notes.md"));
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Assistant"));
    QCoreApplication::setOrganizationName(QStringLiteral("AssistantTeam"));

    int  corpusSize  = 200000;
    bool realPhase   = false;
    int  realSamples = 100;

    for (const QString &a : app.arguments()) {
        if (a.startsWith(QStringLiteral("--n=")))
            corpusSize = a.mid(4).toInt();
        else if (a == QStringLiteral("--real"))
            realPhase = true;
        else if (a.startsWith(QStringLiteral("--real-samples=")))
            realSamples = a.mid(15).toInt();
    }

    runSemanticGolden();
    runSynthetic(corpusSize);
    if (realPhase)
        runReal(realSamples);

    std::printf("\n==== %s：%d 项检查，%d 项失败 ====\n",
                g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
