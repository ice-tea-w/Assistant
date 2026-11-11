// Phase B（文件治理功能一）的单元与端到端用例。
//
// 覆盖四层（治理文档 §1）：
//   L2 采集排除：目录名 / 通配 / 大小 / 工程哨兵（skip / demote / index）
//   L3 文件归类：扩展名表、多段后缀、名称表、用户覆盖
//   L3.5 检索白名单：切换白名单只重建快照，不重扫磁盘
//   L4 查询语法：`:kind` / `ext:` / `in:` / `size:` / `date:` / 短语 / 未知 token 降级

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "core/ConfigManager.h"
#include "core/FileClassifier.h"
#include "core/FileIndex.h"
#include "core/FileKind.h"
#include "core/FileRules.h"
#include "core/FileScanner.h"
#include "core/FileSnapshot.h"
#include "core/QueryParser.h"
#include "database/FileIndexDatabase.h"
#include "ui/SearchResult.h"

class TestFileGovernance : public QObject
{
    Q_OBJECT

private slots:
    void classifierMapsCommonExtensions();
    void classifierHandlesMultiSuffixAndNames();
    void classifierAppliesOverrides();

    void globMatchHandlesWildcards();
    void excludeRulesCheckDirsFilesAndSize();

    void parserReadsGovernanceTokens();
    void parserFallsBackForUnknownToken();
    void parserRejectsMalformedSizeToken();

    void queryEngineFiltersByKindAndExtension();
    void queryEngineFiltersByPathAndSize();
    void queryEngineFilterOnlyListsRecent();
    void queryEngineIgnoresNegationOnlyQuery();

    void scannerSkipsProjectByDefault();
    void scannerDemotesProjectWhenConfigured();
    void scannerIndexesProjectWhenPolicyIsIndex();

    void databaseRoundTripsGovernanceFields();
    void kindFilterAppliesAtQueryTime();
    void configPersistsGovernanceSettings();

private:
    static bool writeFile(const QString &path, const QByteArray &content = "x")
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(content);
        f.close();
        return true;
    }

    static QStringList scannedNames(const QStringList &roots, const ExcludeRuleSet &rules,
                                    FileScanner::Stats *statsOut = nullptr)
    {
        FileScanner::Options options;
        options.rules = &rules;
        options.maxEntries = 10000;

        QVector<FileScanner::Entry> entries;
        const FileScanner::Stats stats = FileScanner::scan(roots, options, &entries);
        if (statsOut)
            *statsOut = stats;

        QStringList names;
        for (const FileScanner::Entry &e : entries)
            names.append(e.name);
        names.sort();
        return names;
    }

    static FileScanner::Entry findEntry(const QVector<FileScanner::Entry> &entries,
                                        const QString &name)
    {
        for (const FileScanner::Entry &e : entries) {
            if (e.name == name)
                return e;
        }
        return {};
    }
};

void TestFileGovernance::classifierMapsCommonExtensions()
{
    const FileClassifier classifier;

    QCOMPARE(classifier.classify(QStringLiteral("report.pdf")), uint16_t(FileKind::Pdf));
    QCOMPARE(classifier.classify(QStringLiteral("Report.DOCX")), uint16_t(FileKind::Document));
    QCOMPARE(classifier.classify(QStringLiteral("photo.JPG")), uint16_t(FileKind::Image));
    QCOMPARE(classifier.classify(QStringLiteral("main.cpp")), uint16_t(FileKind::Code));
    QCOMPARE(classifier.classify(QStringLiteral("song.flac")), uint16_t(FileKind::Audio));
    QCOMPARE(classifier.classify(QStringLiteral("movie.mkv")), uint16_t(FileKind::Video));
    QCOMPARE(classifier.classify(QStringLiteral("setup.msi")), uint16_t(FileKind::Installer));
    QCOMPARE(classifier.classify(QStringLiteral("archive.7z")), uint16_t(FileKind::Archive));
    QCOMPARE(classifier.classify(QStringLiteral("book.epub")), uint16_t(FileKind::Ebook));
    QCOMPARE(classifier.classify(QStringLiteral("weird.qqq")), uint16_t(FileKind::Other));
}

void TestFileGovernance::classifierHandlesMultiSuffixAndNames()
{
    const FileClassifier classifier;

    // 多段后缀按“最长已注册后缀”判，而不是落到单段 `.gz`
    QCOMPARE(classifier.classify(QStringLiteral("backup.tar.gz")), uint16_t(FileKind::Archive));
    QCOMPARE(classifier.classify(QStringLiteral("data.tar.xz")), uint16_t(FileKind::Archive));

    // 名称表优先于扩展名：CMakeLists.txt 是构建文件，不是 txt 文档
    QCOMPARE(classifier.classify(QStringLiteral("CMakeLists.txt")), uint16_t(FileKind::Code));
    QCOMPARE(classifier.classify(QStringLiteral("Makefile")), uint16_t(FileKind::Code));
    QCOMPARE(classifier.classify(QStringLiteral(".gitignore")), uint16_t(FileKind::Data));
    QCOMPARE(classifier.classify(QStringLiteral("LICENSE")), uint16_t(FileKind::Document));

    // 带路径也只取最后一段
    QCOMPARE(classifier.classify(QStringLiteral("C:/work/notes.md")), uint16_t(FileKind::Document));
}

void TestFileGovernance::classifierAppliesOverrides()
{
    FileClassifier classifier;

    QCOMPARE(classifier.classify(QStringLiteral("art.psd")), uint16_t(FileKind::Image));
    classifier.setOverride(QStringLiteral(".psd"), FileKind::Document);
    QCOMPARE(classifier.classify(QStringLiteral("art.psd")), uint16_t(FileKind::Document));

    classifier.addExtraExtension(QStringLiteral("xmind"));
    QCOMPARE(classifier.classify(QStringLiteral("plan.xmind")), uint16_t(FileKind::Other));
}

void TestFileGovernance::globMatchHandlesWildcards()
{
    QVERIFY(ExcludeRuleSet::globMatch(QStringLiteral("*.tmp"), QStringLiteral("a.tmp")));
    QVERIFY(ExcludeRuleSet::globMatch(QStringLiteral("*.TMP"), QStringLiteral("a.tmp")));
    QVERIFY(!ExcludeRuleSet::globMatch(QStringLiteral("*.tmp"), QStringLiteral("a.txt")));
    QVERIFY(ExcludeRuleSet::globMatch(QStringLiteral("~$*"), QStringLiteral("~$report.docx")));
    QVERIFY(ExcludeRuleSet::globMatch(QStringLiteral("a?c"), QStringLiteral("abc")));
    QVERIFY(!ExcludeRuleSet::globMatch(QStringLiteral("a?c"), QStringLiteral("ac")));
    QVERIFY(ExcludeRuleSet::globMatch(QStringLiteral("*report*"), QStringLiteral("annual-report.pdf")));
}

void TestFileGovernance::excludeRulesCheckDirsFilesAndSize()
{
    ExcludeRuleSet rules;
    rules.setDirNames({QStringLiteral("node_modules"), QStringLiteral("build")});
    rules.setFileGlobs({QStringLiteral("*.tmp"), QStringLiteral("~$*")});
    rules.setSizeLimits(10, 1000);

    QVERIFY(rules.excludesDirName(QStringLiteral("node_modules")));
    QVERIFY(rules.excludesDirName(QStringLiteral("NODE_MODULES")));
    QVERIFY(!rules.excludesDirName(QStringLiteral("src")));

    QVERIFY(rules.excludesFile(QStringLiteral("x.tmp"), 100));
    QVERIFY(rules.excludesFile(QStringLiteral("x.tmp"), 100));   // 扩展名规则
    QVERIFY(rules.excludesFile(QStringLiteral("~$a.docx"), 100));
    QVERIFY(!rules.excludesFile(QStringLiteral("a.docx"), 100));

    QVERIFY(rules.excludesFile(QStringLiteral("small.bin"), 9));       // < min
    QVERIFY(!rules.excludesFile(QStringLiteral("ok.bin"), 10));
    QVERIFY(!rules.excludesFile(QStringLiteral("ok.bin"), 1000));
    QVERIFY(rules.excludesFile(QStringLiteral("big.bin"), 1001));      // > max
}

void TestFileGovernance::parserReadsGovernanceTokens()
{
    const QueryParser::Parsed p = QueryParser::parse(
        QStringLiteral(":doc :img ext:pdf,PPTX in:downloads size:>1mb date:7d -tmp \"annual report\""));

    QVERIFY(p.hasKind);
    QCOMPARE(p.kindMask, uint16_t(FileKind::Document | FileKind::Image));
    QCOMPARE(p.extensions, QStringList({QStringLiteral("pdf"), QStringLiteral("pptx")}));
    QCOMPARE(p.inFilters, QStringList({QStringLiteral("downloads")}));

    QVERIFY(p.hasSize);
    QCOMPARE(p.sizeMin, qint64(1024 * 1024 + 1));
    QCOMPARE(p.sizeMax, qint64(-1));

    QVERIFY(p.hasDate);
    QVERIFY(p.dateAfter > 0);

    QCOMPARE(p.negations, QStringList({QStringLiteral("tmp")}));
    QCOMPARE(p.terms, QStringList({QStringLiteral("annual report")}));
}

void TestFileGovernance::parserFallsBackForUnknownToken()
{
    const QueryParser::Parsed p = QueryParser::parse(QStringLiteral(":foo report"));
    QVERIFY(!p.hasKind);
    QCOMPARE(p.terms, QStringList({QStringLiteral(":foo"), QStringLiteral("report")}));

    // 空值 token 也降级
    const QueryParser::Parsed ext = QueryParser::parse(QStringLiteral("ext:"));
    QVERIFY(ext.extensions.isEmpty());
    QCOMPARE(ext.terms, QStringList({QStringLiteral("ext:")}));
}

void TestFileGovernance::parserRejectsMalformedSizeToken()
{
    const QueryParser::Parsed p = QueryParser::parse(QStringLiteral("size:abc"));
    QVERIFY(!p.hasSize);
    QCOMPARE(p.terms, QStringList({QStringLiteral("size:abc")}));

    const QueryParser::Parsed range = QueryParser::parse(QStringLiteral("size:1kb..2kb"));
    QVERIFY(range.hasSize);
    QCOMPARE(range.sizeMin, qint64(1024));
    QCOMPARE(range.sizeMax, qint64(2048));
}

namespace {

std::shared_ptr<const FileSnapshot> makeSyntheticSnapshot()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    FileSnapshot::Builder builder;
    builder.add(QStringLiteral("annual-report.pdf"), QStringLiteral("/docs/annual-report.pdf"),
                2 * 1024 * 1024, now - 100, FileKind::Pdf);
    builder.add(QStringLiteral("holiday.jpg"), QStringLiteral("/pics/holiday.jpg"),
                3 * 1024 * 1024, now - 50, FileKind::Image);
    builder.add(QStringLiteral("notes.txt"), QStringLiteral("/docs/notes.txt"),
                10, now - 10, FileKind::Document);
    builder.add(QStringLiteral("setup.exe"), QStringLiteral("/downloads/setup.exe"),
                50 * 1024 * 1024, now, FileKind::Installer);
    return builder.finish();
}

QStringList titles(const QList<SearchResult> &results)
{
    QStringList out;
    for (const SearchResult &r : results)
        out.append(r.title);
    out.sort();
    return out;
}

} // namespace

void TestFileGovernance::queryEngineFiltersByKindAndExtension()
{
    FileIndex index;
    index.setSnapshot(makeSyntheticSnapshot());

    QCOMPARE(titles(index.query(QStringLiteral(":pdf report"), 20)),
             QStringList({QStringLiteral("annual-report.pdf")}));

    QCOMPARE(titles(index.query(QStringLiteral(":img"), 20)),
             QStringList({QStringLiteral("holiday.jpg")}));

    QCOMPARE(titles(index.query(QStringLiteral("ext:pdf,jpg"), 20)),
             QStringList({QStringLiteral("annual-report.pdf"), QStringLiteral("holiday.jpg")}));

    // 未知 kind token 降级成自由词 → 不命中任何文件
    QVERIFY(index.query(QStringLiteral(":nonsense"), 20).isEmpty());
}

void TestFileGovernance::queryEngineFiltersByPathAndSize()
{
    FileIndex index;
    index.setSnapshot(makeSyntheticSnapshot());

    QCOMPARE(titles(index.query(QStringLiteral("in:docs report"), 10)),
             QStringList({QStringLiteral("annual-report.pdf")}));

    QCOMPARE(titles(index.query(QStringLiteral("in:downloads"), 10)),
             QStringList({QStringLiteral("setup.exe")}));

    QCOMPARE(titles(index.query(QStringLiteral("size:>1mb"), 10)),
             QStringList({QStringLiteral("annual-report.pdf"), QStringLiteral("holiday.jpg"),
                          QStringLiteral("setup.exe")}));

    QCOMPARE(titles(index.query(QStringLiteral("size:<1mb"), 10)),
             QStringList({QStringLiteral("notes.txt")}));
}

void TestFileGovernance::queryEngineFilterOnlyListsRecent()
{
    FileIndex index;
    index.setSnapshot(makeSyntheticSnapshot());

    // 只带过滤 token：按 mtime 降序（setup.exe 最新）
    const QList<SearchResult> all = index.query(QStringLiteral(":pdf"), 1);
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().title, QStringLiteral("annual-report.pdf"));

    // 不限种类（`:all` 也走过滤分支），应当能按 mtime 排出全部 4 条
    const QList<SearchResult> recent = index.query(QStringLiteral(":all"), 10);
    QCOMPARE(recent.size(), 4);
    QCOMPARE(recent.first().title, QStringLiteral("setup.exe"));
}

void TestFileGovernance::queryEngineIgnoresNegationOnlyQuery()
{
    FileIndex index;
    index.setSnapshot(makeSyntheticSnapshot());

    // 只有否定项：与 A4 语义一致，文件不进结果
    QVERIFY(index.query(QStringLiteral("-pdf"), 10).isEmpty());
}

void TestFileGovernance::scannerSkipsProjectByDefault()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QVERIFY(writeFile(dir.path() + "/docs/keep.txt"));
    QVERIFY(writeFile(dir.path() + "/proj/CMakeLists.txt", "project(x)"));
    QVERIFY(writeFile(dir.path() + "/proj/src/main.cpp"));
    QVERIFY(writeFile(dir.path() + "/proj/build/artifact.obj"));

    ExcludeRuleSet rules;   // 默认 projectPolicy = Skip
    QCOMPARE(int(rules.projectPolicy()), int(ExcludeRuleSet::ProjectPolicy::Skip));
    QVERIFY2(rules.projectGuardEnabled(), "default project guard should be on");
    QVERIFY2(rules.isProjectSentinel(QStringLiteral("CMakeLists.txt")),
             "CMakeLists.txt should be a project sentinel");

    FileScanner::Stats stats;
    const QStringList names = scannedNames({dir.path()}, rules, &stats);

    QCOMPARE(names, QStringList({QStringLiteral("keep.txt")}));
    QCOMPARE(stats.projectDirsSkipped, 1);
}

void TestFileGovernance::scannerDemotesProjectWhenConfigured()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QVERIFY(writeFile(dir.path() + "/docs/keep.txt"));
    QVERIFY(writeFile(dir.path() + "/proj/CMakeLists.txt", "project(x)"));
    QVERIFY(writeFile(dir.path() + "/proj/src/main.cpp"));
    QVERIFY(writeFile(dir.path() + "/proj/build/artifact.obj"));

    ExcludeRuleSet rules;
    rules.setProjectPolicy(ExcludeRuleSet::ProjectPolicy::Demote);

    FileScanner::Options options;
    options.rules = &rules;
    QVector<FileScanner::Entry> entries;
    FileScanner::scan({dir.path()}, options, &entries);

    QStringList names;
    for (const FileScanner::Entry &e : entries)
        names.append(e.name);
    names.sort();

    // 工程文件入库，但 build/ 整棵剪掉
    QCOMPARE(names, QStringList({QStringLiteral("CMakeLists.txt"),
                                 QStringLiteral("keep.txt"),
                                 QStringLiteral("main.cpp")}));

    QCOMPARE((findEntry(entries, QStringLiteral("main.cpp")).flags & FileFlags::Project),
             int(FileFlags::Project));
    QCOMPARE((findEntry(entries, QStringLiteral("keep.txt")).flags & FileFlags::Project), 0);
}

void TestFileGovernance::scannerIndexesProjectWhenPolicyIsIndex()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QVERIFY(writeFile(dir.path() + "/docs/keep.txt"));
    QVERIFY(writeFile(dir.path() + "/proj/CMakeLists.txt", "project(x)"));
    QVERIFY(writeFile(dir.path() + "/proj/src/main.cpp"));
    QVERIFY(writeFile(dir.path() + "/proj/build/artifact.obj"));

    ExcludeRuleSet rules;
    rules.setProjectPolicy(ExcludeRuleSet::ProjectPolicy::Index);

    const QStringList names = scannedNames({dir.path()}, rules);
    QCOMPARE(names, QStringList({QStringLiteral("CMakeLists.txt"),
                                 QStringLiteral("artifact.obj"),
                                 QStringLiteral("keep.txt"),
                                 QStringLiteral("main.cpp")}));
}

void TestFileGovernance::databaseRoundTripsGovernanceFields()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/file_index.db";

    FileIndexDatabase::FileRow row;
    row.path     = QStringLiteral("/docs/report.pdf");
    row.name     = QStringLiteral("report.pdf");
    row.size     = 1234;
    row.mtime    = 1700000000;
    row.catMask  = FileKind::Pdf;
    row.flags    = FileFlags::Project;
    row.rootId   = 2;

    FileIndexDatabase::Diff diff;
    diff.added.append(row);

    {
        FileIndexDatabase db(QStringLiteral("gov_roundtrip_1"));
        QVERIFY(db.open(dbPath));
        QVERIFY(db.applyDiff(diff));
        db.close();
    }

    FileIndexDatabase::FileRow loaded;
    {
        FileIndexDatabase db(QStringLiteral("gov_roundtrip_2"));
        QVERIFY(db.open(dbPath));
        const auto rows = db.loadAll();
        QCOMPARE(rows.size(), 1);
        loaded = rows.first();
        db.close();
    }

    QCOMPARE(loaded.path, row.path);
    QCOMPARE(loaded.catMask, row.catMask);
    QCOMPARE(loaded.flags, row.flags);
    QCOMPARE(loaded.rootId, row.rootId);

    // 文件内容没变但种类位变了（分类器升级/overrides）→ 必须进 changed，不能算 unchanged
    FileIndexDatabase::FileRow reclassified = row;
    reclassified.catMask = FileKind::Image;
    const FileIndexDatabase::Diff delta =
        FileIndexDatabase::diff({reclassified}, {loaded});
    QCOMPARE(delta.changed.size(), 1);
    QCOMPARE(delta.unchanged, 0);
}

void TestFileGovernance::kindFilterAppliesAtQueryTime()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QVERIFY(writeFile(dir.path() + "/a.txt"));
    QVERIFY(writeFile(dir.path() + "/b.png"));
    QVERIFY(writeFile(dir.path() + "/c.pdf"));

    FileIndex index;
    index.setRoots({dir.path()});
    index.setDatabasePath(dir.path() + "/file_index.db");
    index.startAsync();

    QTRY_VERIFY_WITH_TIMEOUT(index.isReady(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!index.stats().scanning, 15000);
    QCOMPARE(index.stats().entries, 3);
    QCOMPARE(index.query(QStringLiteral(":doc"), 10).size(), 1);

    // 切换白名单：快照**不变**（不重建、不读 DB、不扫盘），只是下一次查询过滤
    KindFilter onlyImages;
    onlyImages.mode  = KindFilter::Mode::Whitelist;
    onlyImages.kinds = FileKind::Image;
    index.setKindFilter(onlyImages);

    QCOMPARE(index.stats().entries, 3);
    QCOMPARE(index.stats().dbRows, 3);
    QCOMPARE(index.query(QStringLiteral(":img"), 10).size(), 1);
    QVERIFY(index.query(QStringLiteral(":doc"), 10).isEmpty());
    QVERIFY(index.query(QStringLiteral(":all"), 10).size() == 1);   // 只带过滤 token 的最近列表也受限

    // extraExtensions 放行：白名单一个种类都不选，只额外放行 txt
    KindFilter extrasOnly;
    extrasOnly.mode = KindFilter::Mode::Whitelist;
    extrasOnly.extraExtensions.insert(QStringLiteral("txt"));
    index.setKindFilter(extrasOnly);
    QCOMPARE(index.query(QStringLiteral(":all"), 10).size(), 1);
    QCOMPARE(index.query(QStringLiteral(":doc"), 10).size(), 1);

    // 切回 all：同样立即生效
    index.setKindFilter(KindFilter());
    QCOMPARE(index.query(QStringLiteral(":doc"), 10).size(), 1);
    QCOMPARE(index.query(QStringLiteral(":img"), 10).size(), 1);
}

void TestFileGovernance::configPersistsGovernanceSettings()
{
    // QStandardPaths test mode 把 AppConfigLocation 指到测试目录，避免污染真实配置
    QStandardPaths::setTestModeEnabled(true);

    {
        ConfigManager config;
        config.load();
        config.setFileKindFilter(QStringLiteral("whitelist"),
                                 { QStringLiteral("doc"), QStringLiteral("img") });
        config.setFileProjectPolicy(QStringLiteral("demote"));

        QCOMPARE(config.fileKindFilterMode(), QStringLiteral("whitelist"));
        QCOMPARE(config.fileKindFilterKinds(),
                 QStringList({ QStringLiteral("doc"), QStringLiteral("img") }));
        QCOMPARE(config.fileProjectPolicy(), QStringLiteral("demote"));
    }

    {
        // 重新从磁盘读一遍：设置页写入的值必须能回来
        ConfigManager reloaded;
        reloaded.load();
        QCOMPARE(reloaded.fileKindFilterMode(), QStringLiteral("whitelist"));
        QCOMPARE(reloaded.fileKindFilterKinds(),
                 QStringList({ QStringLiteral("doc"), QStringLiteral("img") }));
        QCOMPARE(reloaded.fileProjectPolicy(), QStringLiteral("demote"));
        QCOMPARE(int(reloaded.kindFilter().mode), int(KindFilter::Mode::Whitelist));
        QCOMPARE(int(reloaded.excludeRules().projectPolicy()),
                 int(ExcludeRuleSet::ProjectPolicy::Demote));
    }

    QStandardPaths::setTestModeEnabled(false);
}

QTEST_GUILESS_MAIN(TestFileGovernance)
#include "test_file_governance.moc"
