// A5b 端到端：FileWatcher → FileEventDispatcher → FileIndex → 查询
//
// 赌的是真实链路：临时目录 + 真 ReadDirectoryChangesW + 真 SQLite。
// 验收口径（v0.4-实施清单 §A5）：
//   ① 新建 / 改名 / 删除 ≤100 ms 可在搜索里看到（这里放宽到 3 s 容忍 CI 抖动）
//   ② 溢出后索引仍能收敛 —— 由 FileIndex 的 Overflow → startAsync() 分支保证

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "contract/FileSystemEvent.h"
#include "core/FileEventDispatcher.h"
#include "core/FileIndex.h"
#include "core/FileWatcher.h"
#include "database/FileIndexDatabase.h"
#include "ui/SearchResult.h"

class TestFileIndexIncremental : public QObject
{
    Q_OBJECT

private slots:
    void addedIsSearchable();
    void removedDisappearsFromResults();
    void coldStartPublishesFromDatabase();

    // ---- 2026-11-19 增量校正（dirs.mtime + 24h 闸门）----
    void dirMtimesAreStamped();
    void skipsRescanWithinADay();
    void unchangedTreeSkipsScanAfterDay();
    void incrementalPicksUpNewAndRemovedFiles();

private:
    static bool writeFile(const QString &path, const QByteArray &content = "needle content")
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(content);
        f.close();
        return true;
    }

    static bool resultsContain(const QList<SearchResult> &results, const QString &name)
    {
        for (const SearchResult &r : results) {
            if (r.path.endsWith(name, Qt::CaseInsensitive))
                return true;
        }
        return false;
    }
};

void TestFileIndexIncremental::addedIsSearchable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    FileIndex index;
    index.setRoots({dir.path()});
    index.setDatabasePath(dir.path() + "/file_index.db");

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    index.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });

    QVERIFY(watcher.start({dir.path()}));
    index.startAsync();

    QTRY_VERIFY_WITH_TIMEOUT(index.isReady(), 10000);
    QVERIFY(index.query(QStringLiteral("needle"), 10).isEmpty());

    const QString file = dir.path() + "/needle-report.txt";
    QVERIFY(writeFile(file));

    QTRY_VERIFY_WITH_TIMEOUT(
        resultsContain(index.query(QStringLiteral("needle"), 10), "needle-report.txt"), 5000);

    watcher.stop();
    index.setEventSource(nullptr);
}

void TestFileIndexIncremental::removedDisappearsFromResults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + "/needle-remove.txt";
    QVERIFY(writeFile(file));

    FileIndex index;
    index.setRoots({dir.path()});
    index.setDatabasePath(dir.path() + "/file_index.db");

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    index.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });

    QVERIFY(watcher.start({dir.path()}));
    index.startAsync();

    QTRY_VERIFY_WITH_TIMEOUT(index.isReady(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        resultsContain(index.query(QStringLiteral("remove"), 10), "needle-remove.txt"), 5000);

    QVERIFY(QFile::remove(file));
    QTRY_VERIFY_WITH_TIMEOUT(
        !resultsContain(index.query(QStringLiteral("remove"), 10), "needle-remove.txt"), 5000);

    watcher.stop();
    index.setEventSource(nullptr);
}

void TestFileIndexIncremental::coldStartPublishesFromDatabase()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.path() + "/needle-cold.txt"));

    // DB 必须放在被扫目录之外：放在里面的话，索引自己的 WAL 写入会改父目录的
    // mtime，下一次启动永远判"有变化"，增量闸门就测不出来。
    QTemporaryDir dbDir;
    QVERIFY(dbDir.isValid());
    const QString dbPath = dbDir.path() + "/file_index.db";

    // 第一轮：真扫描，把结果写进 DB
    {
        FileIndex first;
        first.setRoots({dir.path()});
        first.setDatabasePath(dbPath);
        first.startAsync();
        QTRY_VERIFY_WITH_TIMEOUT(first.isReady(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!first.stats().scanning, 15000);
    }
    QVERIFY(QFileInfo::exists(dbPath));

    // 第二轮：冷启动应先从 DB 发布快照（cacheLoaded），此时已可查询
    FileIndex second;
    second.setRoots({dir.path()});
    second.setDatabasePath(dbPath);
    QSignalSpy cacheSpy(&second, &FileIndex::cacheLoaded);

    second.startAsync();

    QTRY_VERIFY_WITH_TIMEOUT(cacheSpy.count() > 0, 15000);
    QVERIFY2(resultsContain(second.query(QStringLiteral("cold"), 10), "needle-cold.txt"),
             "冷启动发布后应立即可查");

    // 校正扫描最终也要收敛
    QTRY_VERIFY_WITH_TIMEOUT(!second.stats().scanning, 15000);
    QVERIFY(resultsContain(second.query(QStringLiteral("cold"), 10), "needle-cold.txt"));
}

/// 把 roots.last_scan 退到"很久以前"，让 24h 闸门放行（模拟隔天启动）。
static bool backdateLastScan(const QString &dbPath, const QString &rootPath)
{
    FileIndexDatabase db(QStringLiteral("file_index_test_backdate"));
    if (!db.open(dbPath))
        return false;
    const bool ok = db.touchRoot(rootPath, 1);
    db.close();
    return ok;
}

void TestFileIndexIncremental::dirMtimesAreStamped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.path() + "/needle-stamp.txt"));
    // DB 必须放在被扫目录之外：放在里面的话，索引自己的 WAL 写入会改父目录的
    // mtime，下一次启动永远判"有变化"，增量闸门就测不出来。
    QTemporaryDir dbDir;
    QVERIFY(dbDir.isValid());
    const QString dbPath = dbDir.path() + "/file_index.db";

    FileIndex index;
    index.setRoots({dir.path()});
    index.setDatabasePath(dbPath);
    index.startAsync();

    QTRY_VERIFY_WITH_TIMEOUT(index.isReady(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!index.stats().scanning, 15000);

    // 全扫之后必须留下"目录 mtime 基线"，否则下次启动又只能全量
    FileIndexDatabase db(QStringLiteral("file_index_test_dirs"));
    QVERIFY(db.open(dbPath));
    const auto dirs = db.loadDirs();
    db.close();

    QVERIFY2(!dirs.isEmpty(), "全扫后 dirs 表应有行");
    bool stamped = false;
    for (const auto &d : dirs) {
        if (d.mtime > 0)
            stamped = true;
    }
    QVERIFY2(stamped, "全扫后目录 mtime 必须被 stamp（否则增量永远判脏）");
}

void TestFileIndexIncremental::skipsRescanWithinADay()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.path() + "/needle-skip.txt"));
    // DB 必须放在被扫目录之外：放在里面的话，索引自己的 WAL 写入会改父目录的
    // mtime，下一次启动永远判"有变化"，增量闸门就测不出来。
    QTemporaryDir dbDir;
    QVERIFY(dbDir.isValid());
    const QString dbPath = dbDir.path() + "/file_index.db";

    {
        FileIndex first;
        first.setRoots({dir.path()});
        first.setDatabasePath(dbPath);
        first.startAsync();
        QTRY_VERIFY_WITH_TIMEOUT(first.isReady(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!first.stats().scanning, 15000);
    }

    // 第二次启动：距上次不到 24h → 一条都不枚举，缓存快照继续有效
    FileIndex second;
    second.setRoots({dir.path()});
    second.setDatabasePath(dbPath);
    second.setScanDeferEnabled(false);          // 测试不等"用户空闲"
    QSignalSpy cacheSpy(&second, &FileIndex::cacheLoaded);
    QSignalSpy skipSpy(&second, &FileIndex::scanSkipped);

    second.startAsync();

    QTRY_VERIFY_WITH_TIMEOUT(cacheSpy.count() > 0, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(skipSpy.count() > 0, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!second.stats().scanning, 15000);

    QVERIFY2(resultsContain(second.query(QStringLiteral("skip"), 10), "needle-skip.txt"),
             "跳过校正后缓存快照必须仍然可查");
    QVERIFY(second.stats().entries >= 1);
}

void TestFileIndexIncremental::unchangedTreeSkipsScanAfterDay()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.path() + "/needle-quiet.txt"));
    // DB 必须放在被扫目录之外：放在里面的话，索引自己的 WAL 写入会改父目录的
    // mtime，下一次启动永远判"有变化"，增量闸门就测不出来。
    QTemporaryDir dbDir;
    QVERIFY(dbDir.isValid());
    const QString dbPath = dbDir.path() + "/file_index.db";

    {
        FileIndex first;
        first.setRoots({dir.path()});
        first.setDatabasePath(dbPath);
        first.startAsync();
        QTRY_VERIFY_WITH_TIMEOUT(first.isReady(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!first.stats().scanning, 15000);
    }

    // 隔天启动，但目录 mtime 一个都没变 → 只 stat，不枚举
    QVERIFY(backdateLastScan(dbPath, dir.path()));

    FileIndex again;
    again.setRoots({dir.path()});
    again.setDatabasePath(dbPath);
    again.setScanDeferEnabled(false);
    QSignalSpy skipSpy(&again, &FileIndex::scanSkipped);

    again.startAsync();

    QTRY_VERIFY_WITH_TIMEOUT(skipSpy.count() > 0, 20000);
    QVERIFY2(skipSpy.first().at(0).toString().contains(QStringLiteral("未变")),
             qPrintable(skipSpy.first().at(0).toString()));
    QVERIFY(resultsContain(again.query(QStringLiteral("quiet"), 10), "needle-quiet.txt"));
}

void TestFileIndexIncremental::incrementalPicksUpNewAndRemovedFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString keep = dir.path() + "/needle-keep.txt";
    const QString gone = dir.path() + "/needle-gone.txt";
    QVERIFY(writeFile(keep));
    QVERIFY(writeFile(gone));
    // DB 必须放在被扫目录之外：放在里面的话，索引自己的 WAL 写入会改父目录的
    // mtime，下一次启动永远判"有变化"，增量闸门就测不出来。
    QTemporaryDir dbDir;
    QVERIFY(dbDir.isValid());
    const QString dbPath = dbDir.path() + "/file_index.db";

    {
        FileIndex first;
        first.setRoots({dir.path()});
        first.setDatabasePath(dbPath);
        first.startAsync();
        QTRY_VERIFY_WITH_TIMEOUT(first.isReady(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!first.stats().scanning, 15000);
    }

    // 磁盘变化 + 把上次扫描时间退到 1 秒（1970），让 24h 闸门放行
    QVERIFY(QFile::remove(gone));
    QVERIFY(writeFile(dir.path() + "/needle-fresh.txt"));
    QVERIFY(backdateLastScan(dbPath, dir.path()));

    FileIndex again;
    again.setRoots({dir.path()});
    again.setDatabasePath(dbPath);
    again.setScanDeferEnabled(false);
    QSignalSpy readySpy(&again, &FileIndex::indexReady);
    again.startAsync();

    // 注意：coldLoad 结束时就 m_scanning=false，但那次校正扫描还没发布。
    // 所以要等 indexReady（扫描完成），不能只看 scanning 标志。
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() > 0, 20000);

    QVERIFY2(resultsContain(again.query(QStringLiteral("fresh"), 10), "needle-fresh.txt"),
             "新增文件必须进结果（目录 mtime 变了 → 重扫）");
    QVERIFY2(!resultsContain(again.query(QStringLiteral("gone"), 10), "needle-gone.txt"),
             "删除的文件必须从结果消失");
    QVERIFY2(resultsContain(again.query(QStringLiteral("keep"), 10), "needle-keep.txt"),
             "未变的文件必须还在");
}

QTEST_GUILESS_MAIN(TestFileIndexIncremental)
#include "test_file_index_incremental.moc"
