/// A2 验收③：**v2（`FileIndex`）与旧版的查询结果逐条一致**。
///
/// 两个阶段：
///
/// **① 合成语料（默认 20 万条，`--n=NNN` 可改）** —— 直接注入 v2 的快照，
/// 与「旧 `FileSearcher::search()` 算法」的参考实现在**同一候选集**上逐条比对。
/// 这是 `v0.4-实施清单.md` §A2 验收③ 的字面要求。
/// 之所以必须是"同一候选集"：v2 刻意**不再套 2 万条任意截断**（P0-3：本机
/// Desktop 就有 3.5 万文件，旧上限让 Documents/Downloads 一个文件都进不来），
/// 所以真机上两者的候选集本来就不同 —— 那是设计意图，不是回归。这里比的是
/// **查询语义**：同样的命中、同样的分数、同样的顺序、同样的条数上限。
///
/// **② 真机数据（`--real`）** —— 把 v2 的上限压到 20000 条，于是 v2 与旧
/// `FileSearcher` 落在同一候选集上，直接拿**真实磁盘**跑同一批查询。这一阶段
/// 证明的是两件事：A2 的遍历与 A1 的遍历收敛到同一集合，且 `FileIndex::query`
/// 与 `FileSearcher::search` 在真实数据上等价。
///
/// 退出码：0 = 全过；1 = 有差异（差异逐条打印，便于定位）。

#include "core/FileIndex.h"
#include "core/FileSnapshot.h"
#include "core/FileSearcher.h"
#include "core/FuzzyMatcher.h"
#include "ui/SearchResult.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileInfoList>
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

/// 真机等价比对用的上限：与旧 `FileSearcher` 的 `kMaxIndexedFiles` 相同，
/// 这样两个引擎才落在同一候选集上。
constexpr int kComparableLimit = 20000;

void report(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  FAIL  %s\n", what.toUtf8().constData());
    }
}

// ---------------------------------------------------------------------------
// 参考实现：旧 FileSearcher::search() 算法的逐行转写
// （src/core/FileSearcher.cpp：线性扫描 + FuzzyMatcher + 分数降序 +
//   名字大小写不敏感序 + 截断到 limit）
// ---------------------------------------------------------------------------

struct RefHit
{
    QString name;
    QString path;
    int     score = 0;
};

QList<RefHit> legacyReference(const std::vector<QString> &names,
                              const QString &query,
                              int limit)
{
    QList<RefHit> matches;
    if (query.trimmed().isEmpty() || limit <= 0)
        return matches;

    matches.reserve(1024);
    for (const QString &name : names) {
        const int s = FuzzyMatcher::score(query, name);
        if (s > 0)
            matches.append({name, QStringLiteral("C:/corpus/") + name, s});
    }

    std::sort(matches.begin(), matches.end(), [](const RefHit &a, const RefHit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    if (matches.size() > limit)
        matches.erase(matches.begin() + limit, matches.end());
    return matches;
}

/// 逐条比对 v2 的结果与参考实现（顺序也必须是同一顺序）。
void compareAgainstReference(const std::vector<QString> &names,
                             FileIndex &index,
                             const QString &query,
                             int limit)
{
    const QList<RefHit>          want = legacyReference(names, query, limit);
    const QList<SearchResult>    got  = index.query(query, limit);

    const QString tag = QStringLiteral("query='%1' limit=%2").arg(query).arg(limit);

    if (want.size() != got.size()) {
        report(false, QStringLiteral("%1 — 条数 %2 vs %3")
                         .arg(tag).arg(want.size()).arg(got.size()));
        return;
    }

    for (int i = 0; i < want.size(); ++i) {
        const RefHit       &w = want.at(i);
        const SearchResult &g = got.at(i);
        if (w.name == g.title && w.score == g.score &&
            g.id == QStringLiteral("file://") + w.path &&
            g.subtitle == w.path && g.path == w.path &&
            g.type == SearchResult::File && g.priority == 1)
            continue;

        report(false, QStringLiteral("%1 — 第 %2 条不一致：want(%3,%4) got(%5,%6)")
                         .arg(tag).arg(i).arg(w.name).arg(w.score)
                         .arg(g.title).arg(g.score));
        return;
    }

    report(true, tag);
}

// ---------------------------------------------------------------------------
// 阶段①：合成语料
// ---------------------------------------------------------------------------

/// 程序化语料（可重复）：不同深度、非 ASCII 约 10%、长名字若干。
/// 名字尾部拼 `_<i>` 保证**唯一** —— `std::sort` 不是稳定排序，
/// 名字（大小写不敏感）相同的条目之间顺序未定义，会让比对假失败。
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

int runSynthetic(int n)
{
    std::printf("\n[phase 1] 合成语料 N=%d\n", n);

    const std::vector<QString> names = makeCorpus(n, 20261015u);

    // 注入 v2 快照（不碰磁盘）：包成 FileSnapshot::Builder 的正常路径
    FileSnapshot::Builder builder;
    builder.reserve(n);
    for (const QString &name : names)
        builder.add(name, QStringLiteral("C:/corpus/") + name, 1024, 1700000000);

    std::shared_ptr<FileSnapshot> snap = builder.finish();

    FileIndex index;
    index.setSnapshot(snap);

    // ---- 快照自身的不变量 ----
    report(snap->size() == n,
           QStringLiteral("快照 size() == N（%1）").arg(snap->size()));
    {
        bool lowerOk = true, nonEmpty = true;
        for (int i = 0; i < snap->size() && lowerOk; ++i) {
            if (snap->lowerAt(i) != snap->nameAt(i).toLower()) lowerOk = false;
            if (snap->nameAt(i).isEmpty() || snap->pathAt(i).isEmpty()) nonEmpty = false;
        }
        report(lowerOk,   QStringLiteral("lowerAt(i) == nameAt(i).toLower() 全表"));
        report(nonEmpty,  QStringLiteral("name/path 全表非空"));
        report(snap->mtimeAt(0) == 1700000000 && snap->sizeAt(0) == 1024,
               QStringLiteral("size/mtime 原样带出"));
    }

    // ---- 查询语义比对 ----
    // 每次比对都是一遍完整的全表扫描，而当前 `FuzzyMatcher` 每次调用都要
    // toLower + 可能的子串分配（A4 要解决的 P1-2）：全量跑 75 遍太贵，
    // 所以主比对只用生产值的 limit=20，边界 limit 另用代表性查询覆盖。
    const std::vector<QString> queries = {
        QStringLiteral("report"),                       // 词
        QStringLiteral("rep"),                          // 前缀
        QStringLiteral("port"),                         // 子串
        QStringLiteral("chr"),                          // 子串（启动器场景）
        QStringLiteral("report_1"),                     // 词 + 数字
        QString::fromUtf8("报告"),                       // 中文
        QString::fromUtf8("预算"),                       // 中文
        QStringLiteral("a"),                            // 极短（候选最多）
        QStringLiteral("z"),                            // 可能无命中
        QStringLiteral("xyzzy_no_such_file"),           // 必无命中
        QString(),                                      // 空查询
        QStringLiteral("   "),                          // 空白查询
        names[0],                                       // 精确命中（score 1000）
        names[names.size() - 1],                        // 精确命中（尾部）
        names[1].left(12),                              // 长查询
    };

    QElapsedTimer timer;
    timer.start();
    int scans = 0;
    for (const QString &q : queries) {
        compareAgainstReference(names, index, q, 20);
        ++scans;
    }

    // limit 边界：0 / 1 / 5 / 超过命中数
    const std::vector<QString> limitQueries = {
        QStringLiteral("a"), QStringLiteral("report"), QString(),
    };
    for (int limit : { 0, 1, 5, 1000 }) {
        for (const QString &q : limitQueries) {
            compareAgainstReference(names, index, q, limit);
            ++scans;
        }
    }
    const qint64 queryMs = timer.elapsed();

    // 单遍全表扫描的量级（给 A4 的 baseline 一个参照；不是正式 baseline 报告）
    QElapsedTimer single;
    single.start();
    (void)index.query(QStringLiteral("report"), 20);
    const qint64 oneScanMs = single.elapsed();

    std::printf("  %d 遍全表比对：%lld ms   单遍全表扫描 N=%d：%lld ms\n",
                scans, static_cast<long long>(queryMs), n,
                static_cast<long long>(oneScanMs));
    return 0;
}

// ---------------------------------------------------------------------------
// 阶段②：真机数据（v2 上限压到 20000，与旧引擎同候选集）
// ---------------------------------------------------------------------------

/// 从 Desktop 取真实文件名当查询。
///
/// 必须**自己控制递归**：`QDirIterator::Subdirectories` 会跟着 junction 一路钻
/// 下去 —— 那正是 A1 的 P0-1。本机 Desktop 顶层只有 `desktop.ini` 一个文件，
/// 2 万多个文件全在子目录里，所以只取顶层等于一条查询都取不到。
QStringList harvestQueries(int n)
{
    QStringList out;
    const QString desktop =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty())
        return out;

    QStringList stack{ desktop };
    while (!stack.isEmpty() && out.size() < n) {
        const QString dir = stack.takeLast();
        const QFileInfoList entries =
            QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                    QDir::NoSort);
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

            out << name;                                   // 精确命中（score 1000）
            out << name.left(qMax(3, name.length() / 2));  // 前缀命中（score 900）
        }
    }
    return out;
}

int runReal(int sampleQueries, int entryLimit)
{
    std::printf("\n[phase 2] 真机数据（v2 limit=%d vs 旧 FileSearcher）\n", entryLimit);

    // 旧引擎：同步建索引（A1 行为）
    QElapsedTimer legacyTimer;
    legacyTimer.start();
    const int legacyCount = FileSearcher::buildIndex();
    const qint64 legacyMs = legacyTimer.elapsed();

    // v2：后台扫描。压到 20000 才有"同一候选集"可比；
    // 传 0 则用默认硬上限（跑全量），此时只测规模与耗时，不做等价比对。
    const bool comparable = entryLimit > 0;

    FileIndex index;
    index.setEntryLimit(entryLimit);

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

    // 单遍全表扫描（v2 路径）——切默认引擎前必须知道的数字
    {
        QElapsedTimer one;
        one.start();
        (void)index.query(QStringLiteral("report"), 20);
        std::printf("  v2 单遍全表扫描 %d 条：%lld ms\n", st.entries,
                    static_cast<long long>(one.elapsed()));
    }

    if (!comparable) {
        std::printf("  （limit=0 → 全量索引，只测规模/耗时；等价比对见 limit=%d 的运行）\n",
                    kComparableLimit);
        return 0;
    }

    report(st.entries == legacyCount,
           QStringLiteral("候选集规模一致（legacy %1 vs v2 %2）")
               .arg(legacyCount).arg(st.entries));

    const QStringList queries = harvestQueries(sampleQueries);
    std::printf("  用 %d 条真实查询比对（含精确/前缀两档）\n", static_cast<int>(queries.size()));

    int compared = 0;
    for (const QString &q : queries) {
        const QList<FileSearcher::Entry> want = FileSearcher::search(q, 20);
        const QList<SearchResult>        got  = index.query(q, 20);

        ++compared;
        if (want.size() != got.size()) {
            report(false, QStringLiteral("query='%1' 条数 %2 vs %3")
                             .arg(q).arg(want.size()).arg(got.size()));
            continue;
        }
        bool same = true;
        for (int i = 0; i < want.size() && same; ++i) {
            same = want.at(i).name == got.at(i).title &&
                   want.at(i).score == got.at(i).score &&
                   got.at(i).id == QStringLiteral("file://") + want.at(i).path;
        }
        report(same, QStringLiteral("query='%1'（%2 条）").arg(q).arg(want.size()));
    }

    std::printf("  真机比对 %d 条查询完成\n", compared);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Assistant"));
    QCoreApplication::setOrganizationName(QStringLiteral("AssistantTeam"));

    int  corpusSize   = 200000;
    bool realPhase    = false;
    int  realSamples  = 100;
    int  realLimit    = kComparableLimit;

    const QStringList args = app.arguments();
    for (const QString &a : args) {
        if (a.startsWith(QStringLiteral("--n=")))
            corpusSize = a.mid(4).toInt();
        else if (a == QStringLiteral("--real"))
            realPhase = true;
        else if (a.startsWith(QStringLiteral("--real-samples=")))
            realSamples = a.mid(15).toInt();
        else if (a.startsWith(QStringLiteral("--real-limit=")))
            realLimit = a.mid(13).toInt();
    }

    runSynthetic(corpusSize);
    if (realPhase)
        runReal(realSamples, realLimit);

    std::printf("\n==== %s：%d 项检查，%d 项失败 ====\n",
                g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
