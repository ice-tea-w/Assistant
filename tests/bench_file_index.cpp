/// A4 baseline 工具：在同一份合成语料上，把**新引擎**（QueryEngine / ScoreEngine）
/// 与**旧实现**（`FuzzyMatcher::score` + 全排序 + 截断，逐行转写自 `FileSearcher::search`）
/// 放在一起量。
///
/// ```
///   assistant_bench                      # 默认 N = 2 万 / 5 万 / 10 万 / 20 万
///   assistant_bench --sizes=20000,200000 --queries=20 --repeat=3
///   assistant_bench --sizes=20000 --db=<file_index.db>   # 额外量一次「冷启动可查询」
/// ```
///
/// 刻意分开两档查询长度（1–3 / 4–8），因为它们的候选集大小差一个数量级 ——
/// 混在一起的平均值没有意义（架构文档 §9 第一步的要求）。
///
/// 旧实现只在**每个规模上各跑 1 条查询**：它在 20 万条上单条就要十几秒，
/// 跑 20 条 × 2 档会让这个工具失去可用性。所以旧引擎的数字是"单样本延迟"，
/// 只用来算新旧比值，不参与 P50/P99 统计。

#include "core/FileSnapshot.h"
#include "core/FuzzyMatcher.h"
#include "core/QueryEngine.h"
#include "core/QueryParser.h"
#include "core/ScoreEngine.h"
#include "core/SnapshotManager.h"
#include "core/Logger.h"
#include "database/FileIndexDatabase.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 语料
// ---------------------------------------------------------------------------

struct Corpus
{
    std::shared_ptr<FileSnapshot> snapshot;
    std::vector<QString>          names;      // 原始大小写（旧引擎要用）
    qint64                        buildUs = 0;
};

/// 程序化语料：不同深度、约 10% 非 ASCII、长名字若干，名字唯一（非稳定排序也不抖）。
std::vector<QString> makeNames(int n, unsigned seed)
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
    std::vector<QString> names;
    names.reserve(static_cast<std::size_t>(n));

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
        names.push_back(name);
    }
    return names;
}

Corpus buildCorpus(int n)
{
    Corpus corpus;
    corpus.names = makeNames(n, 20261022u);

    QElapsedTimer timer;
    timer.start();

    FileSnapshot::Builder builder;
    builder.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QString &name = corpus.names[static_cast<std::size_t>(i)];
        // 造一点路径深度差异，避免所有条目共享同一个 dir
        const QString path = QStringLiteral("C:/bench/d%1/s%2/").arg(i % 97).arg(i % 13) + name;
        builder.add(name, path, 1024 + i, 1700000000 + i);
    }
    corpus.snapshot = builder.finish();
    corpus.buildUs  = timer.nsecsElapsed() / 1000;
    return corpus;
}

// ---------------------------------------------------------------------------
// 旧实现（逐行转写自 src/core/FileSearcher.cpp 的 search()）
// ---------------------------------------------------------------------------

struct LegacyHit
{
    int index = -1;
    int score = 0;
};

QList<LegacyHit> legacyQuery(const FileSnapshot &snap, const QString &query, int limit)
{
    QList<LegacyHit> hits;
    hits.reserve(1024);

    for (int i = 0; i < snap.size(); ++i) {
        const int s = FuzzyMatcher::score(query, snap.nameAt(i));   // 旧引擎传原始大小写
        if (s > 0)
            hits.append({ i, s });
    }

    std::sort(hits.begin(), hits.end(), [&snap](const LegacyHit &a, const LegacyHit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return snap.nameAt(a.index).compare(snap.nameAt(b.index), Qt::CaseInsensitive) < 0;
    });

    if (hits.size() > limit)
        hits.erase(hits.begin() + limit, hits.end());
    return hits;
}

// ---------------------------------------------------------------------------
// 查询集
// ---------------------------------------------------------------------------

QStringList makeQueries(const std::vector<QString> &names, int count, int minLen, int maxLen)
{
    QStringList queries;
    const int n = static_cast<int>(names.size());
    for (int k = 0; k < count && n > 0; ++k) {
        const QString &name = names[static_cast<std::size_t>((k * 7919 + 13) % n)];
        const int len = minLen + (k % (maxLen - minLen + 1));
        if (name.size() >= len)
            queries << name.left(len);
    }
    return queries;
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------

qint64 percentile(const QVector<qint64> &sorted, double p)
{
    if (sorted.isEmpty())
        return 0;
    const double idx = p * (sorted.size() - 1);
    const int    lo  = static_cast<int>(idx);
    const int    hi  = std::min(lo + 1, static_cast<int>(sorted.size()) - 1);
    const double f   = idx - lo;
    return static_cast<qint64>(sorted[lo] * (1.0 - f) + sorted[hi] * f);
}

/// 快照内存**估算**（不含分配器碎片/对齐浪费）：三个 QString 各一份
/// QStringData 头(24B) + 2 字节/字符的缓冲，加条目本体。
qint64 estimateSnapshotBytes(const FileSnapshot &snap)
{
    qint64 bytes = static_cast<qint64>(snap.size()) * static_cast<qint64>(sizeof(FileSnapshot::FileEntry));
    constexpr qint64 kQStringHeader = 24;
    for (int i = 0; i < snap.size(); ++i) {
        bytes += 3 * kQStringHeader;
        bytes += 2 * static_cast<qint64>(snap.lowerAt(i).size());
        bytes += 2 * static_cast<qint64>(snap.nameAt(i).size());
        bytes += 2 * static_cast<qint64>(snap.pathAt(i).size());
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// 单规模测量
// ---------------------------------------------------------------------------

struct SizeResult
{
    int    n = 0;
    qint64 buildUs = 0;
    qint64 bytes   = 0;

    // 新引擎：短查询 / 长查询
    qint64 newP50Short = 0, newP95Short = 0, newP99Short = 0;
    qint64 newP50Long  = 0, newP95Long  = 0, newP99Long  = 0;
    qint64 newHitsShort = 0, newHitsLong = 0;

    // 旧引擎（每档 1 条查询，单样本）
    qint64 legacyShortUs = 0, legacyLongUs = 0;
    bool   legacySampled = false;
};

SizeResult measureSize(int n, int queryCount, int repeat)
{
    SizeResult result;
    result.n = n;

    const Corpus corpus = buildCorpus(n);
    result.buildUs = corpus.buildUs;
    result.bytes   = estimateSnapshotBytes(*corpus.snapshot);

    SnapshotManager snapshots;
    snapshots.publish(corpus.snapshot);
    const QueryEngine engine(snapshots);

    const QStringList shortQueries = makeQueries(corpus.names, queryCount, 1, 3);
    const QStringList longQueries  = makeQueries(corpus.names, queryCount, 4, 8);

    auto timeQueries = [&](const QStringList &queries, qint64 *p50, qint64 *p95, qint64 *p99,
                           qint64 *hitSum) {
        QVector<qint64> samples;
        samples.reserve(queries.size());
        qint64 hits = 0;

        for (const QString &q : queries) {
            qint64 best = -1;
            for (int r = 0; r < repeat; ++r) {
                QElapsedTimer timer;
                timer.start();
                const QList<QueryEngine::Match> matches = engine.query(q, 20);
                const qint64 us = timer.nsecsElapsed() / 1000;
                hits += matches.size();
                if (best < 0 || us < best)
                    best = us;
            }
            if (best >= 0)
                samples.append(best);
        }

        std::sort(samples.begin(), samples.end());
        *p50    = percentile(samples, 0.50);
        *p95    = percentile(samples, 0.95);
        *p99    = percentile(samples, 0.99);
        *hitSum = queries.isEmpty() ? 0 : hits / (queries.size() * repeat);
    };

    timeQueries(shortQueries, &result.newP50Short, &result.newP95Short, &result.newP99Short,
                &result.newHitsShort);
    timeQueries(longQueries, &result.newP50Long, &result.newP95Long, &result.newP99Long,
                &result.newHitsLong);

    // 旧引擎：每档只量 1 条（见文件头说明）
    if (!shortQueries.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        const QList<LegacyHit> hits = legacyQuery(*corpus.snapshot, shortQueries.first(), 20);
        result.legacyShortUs = timer.nsecsElapsed() / 1000;
        (void)hits;
    }
    if (!longQueries.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        const QList<LegacyHit> hits = legacyQuery(*corpus.snapshot, longQueries.first(), 20);
        result.legacyLongUs = timer.nsecsElapsed() / 1000;
        (void)hits;
    }
    result.legacySampled = true;

    std::printf("  完成 N=%-7d 快照 %.0f ms / 估算 %.1f MB\n",
                n, result.buildUs / 1000.0,
                static_cast<double>(result.bytes) / (1024.0 * 1024.0));
    return result;
}

// ---------------------------------------------------------------------------
// 冷启动可查询（A6 的预览）：DB 载入 → 建快照 → 一次查询
// ---------------------------------------------------------------------------

void measureColdStart(const QString &dbPath)
{
    std::printf("\n[cold start] db=%s\n", dbPath.toUtf8().constData());
    if (!QFileInfo::exists(dbPath)) {
        std::printf("  跳过：库不存在\n");
        return;
    }

    FileIndexDatabase db;
    if (!db.open(dbPath)) {
        std::printf("  跳过：打不开库（%s）\n", db.lastError().toUtf8().constData());
        return;
    }

    QElapsedTimer timer;
    timer.start();
    const QVector<FileIndexDatabase::FileRow> rows = db.loadAll();
    const qint64 loadUs = timer.nsecsElapsed() / 1000;
    db.close();

    timer.restart();
    FileSnapshot::Builder builder;
    builder.reserve(static_cast<int>(rows.size()));
    for (const FileIndexDatabase::FileRow &row : rows)
        builder.add(row.name, row.path, row.size, row.mtime);
    std::shared_ptr<FileSnapshot> snapshot = builder.finish();
    const qint64 buildUs = timer.nsecsElapsed() / 1000;

    SnapshotManager snapshots;
    snapshots.publish(snapshot);
    const QueryEngine engine(snapshots);

    timer.restart();
    const QList<QueryEngine::Match> first = engine.query(QStringLiteral("report"), 20);
    const qint64 queryUs = timer.nsecsElapsed() / 1000;

    std::printf("  rows=%d  loadAll=%.1f ms  buildSnapshot=%.1f ms  首次查询=%.1f ms"
                "（命中 %d）\n",
                static_cast<int>(rows.size()), loadUs / 1000.0, buildUs / 1000.0,
                queryUs / 1000.0, static_cast<int>(first.size()));
    std::printf("  → 冷启动到「可查询」≈ %.1f ms（DB 载入 + 建快照）\n",
                (loadUs + buildUs) / 1000.0);
    std::printf("  快照内存估算：%.1f MB\n",
                static_cast<double>(estimateSnapshotBytes(*snapshot)) / (1024.0 * 1024.0));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Assistant"));
    QCoreApplication::setOrganizationName(QStringLiteral("AssistantTeam"));

    QVector<int> sizes = { 20000, 50000, 100000, 200000 };
    int  queryCount = 20;
    int  repeat     = 3;
    QString dbPath;

    for (const QString &arg : app.arguments()) {
        if (arg.startsWith(QStringLiteral("--sizes="))) {
            sizes.clear();
            for (const QString &s : arg.mid(8).split(QLatin1Char(','), Qt::SkipEmptyParts))
                sizes.append(s.toInt());
        } else if (arg.startsWith(QStringLiteral("--queries="))) {
            queryCount = arg.mid(10).toInt();
        } else if (arg.startsWith(QStringLiteral("--repeat="))) {
            repeat = std::max(1, arg.mid(9).toInt());
        } else if (arg.startsWith(QStringLiteral("--db="))) {
            dbPath = arg.mid(5);
        }
    }

    std::printf("A4 baseline：新引擎 QueryEngine/ScoreEngine vs 旧 FuzzyMatcher 全排序\n");
    std::printf("每档查询数=%d，每查询重复=%d 次取最快；旧引擎每档只量 1 条（单样本）\n\n",
                queryCount, repeat);

    QVector<SizeResult> results;
    for (int n : sizes) {
        results.append(measureSize(n, queryCount, repeat));
    }

    // ---- markdown 表：查询延迟（新引擎，P50/P95/P99） ----
    std::printf("\n### 查询延迟（新引擎，单位 ms）\n\n");
    std::printf("| N | 档 | P50 | P95 | P99 | 平均命中数 |\n");
    std::printf("|---|---|---|---|---|---|\n");
    for (const SizeResult &r : results) {
        std::printf("| %d | 1–3 | %.3f | %.3f | %.3f | %lld |\n", r.n,
                    r.newP50Short / 1000.0, r.newP95Short / 1000.0, r.newP99Short / 1000.0,
                    static_cast<long long>(r.newHitsShort));
        std::printf("| %d | 4–8 | %.3f | %.3f | %.3f | %lld |\n", r.n,
                    r.newP50Long / 1000.0, r.newP95Long / 1000.0, r.newP99Long / 1000.0,
                    static_cast<long long>(r.newHitsLong));
    }

    // ---- markdown 表：新旧对比 ----
    std::printf("\n### 新旧对比（单查询延迟，ms）\n\n");
    std::printf("| N | 旧·1–3 | 新·1–3 | 倍数 | 旧·4–8 | 新·4–8 | 倍数 |\n");
    std::printf("|---|---|---|---|---|---|---|\n");
    for (const SizeResult &r : results) {
        const double newShort = r.newP50Short / 1000.0;
        const double newLong  = r.newP50Long / 1000.0;
        const double oldShort = r.legacyShortUs / 1000.0;
        const double oldLong  = r.legacyLongUs / 1000.0;
        std::printf("| %d | %.3f | %.3f | %.1f× | %.3f | %.3f | %.1f× |\n",
                    r.n, oldShort, newShort,
                    newShort > 0 ? oldShort / newShort : 0.0,
                    oldLong, newLong,
                    newLong > 0 ? oldLong / newLong : 0.0);
    }

    // ---- markdown 表：快照 ----
    std::printf("\n### 快照（构建 + 内存估算）\n\n");
    std::printf("| N | 构建 ms | 内存估算 MB |\n|---|---|---|\n");
    for (const SizeResult &r : results)
        std::printf("| %d | %.1f | %.1f |\n", r.n, r.buildUs / 1000.0,
                    static_cast<double>(r.bytes) / (1024.0 * 1024.0));

    if (!dbPath.isEmpty())
        measureColdStart(dbPath);

    return 0;
}
