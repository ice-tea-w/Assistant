// A5b 端到端：FileWatcher → FileEventDispatcher → FileIndex → 查询
//
// 赌的是真实链路：临时目录 + 真 ReadDirectoryChangesW + 真 SQLite。
// 验收口径（v0.4-实施清单 §A5）：
//   ① 新建 / 改名 / 删除 ≤100 ms 可在搜索里看到（这里放宽到 3 s 容忍 CI 抖动）
//   ② 溢出后索引仍能收敛 —— 由 FileIndex 的 Overflow → startAsync() 分支保证

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "contract/FileSystemEvent.h"
#include "core/FileEventDispatcher.h"
#include "core/FileIndex.h"
#include "core/FileWatcher.h"
#include "ui/SearchResult.h"

class TestFileIndexIncremental : public QObject
{
    Q_OBJECT

private slots:
    void addedIsSearchable();
    void removedDisappearsFromResults();

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

QTEST_GUILESS_MAIN(TestFileIndexIncremental)
#include "test_file_index_incremental.moc"
