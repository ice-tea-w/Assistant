// FileWatcher / FileEventDispatcher 单测（A5）
//
// 用 QTemporaryDir 造真实目录，赌的是 ReadDirectoryChangesW 的真实行为，不是桩。
// 事件有合并窗口（默认 100 ms），所以断言一律用 QTRY_VERIFY_WITH_TIMEOUT。

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <stdexcept>
#include <vector>

#include "contract/FileSystemEvent.h"
#include "core/FileEventDispatcher.h"
#include "core/FileWatcher.h"

class TestFileWatcher : public QObject
{
    Q_OBJECT

private slots:
    void addedAndRemoved();
    void renamed();
    void modified();
    void coalescingDeduplicates();
    void dispatcherSubscribePublish();
    void dispatcherIsolatesThrowingSubscriber();

private:
    static bool writeFile(const QString &path, const QByteArray &content = "hello")
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(content);
        f.close();
        return true;
    }

    static bool hasEvent(const QVector<FileSystemEvent> &events,
                         FileSystemEvent::Type type,
                         const QString &pathSuffix)
    {
        for (const FileSystemEvent &ev : events) {
            if (ev.type == type && ev.path.endsWith(pathSuffix, Qt::CaseInsensitive))
                return true;
        }
        return false;
    }
};

void TestFileWatcher::addedAndRemoved()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    FileWatcher watcher;
    watcher.setCoalesceMs(30);
    QVector<FileSystemEvent> got;
    connect(&watcher, &FileWatcher::batchReady, this,
            [&got](const QVector<FileSystemEvent> &batch) { got += batch; });
    QVERIFY(watcher.start({dir.path()}));

    const QString file = dir.path() + "/alpha.txt";
    QVERIFY(writeFile(file));
    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(got, FileSystemEvent::Added, "alpha.txt"), 4000);

    got.clear();
    QVERIFY(QFile::remove(file));
    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(got, FileSystemEvent::Removed, "alpha.txt"), 4000);

    watcher.stop();
    QVERIFY(!watcher.isRunning());
}

void TestFileWatcher::renamed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString from = dir.path() + "/before.txt";
    QVERIFY(writeFile(from));

    FileWatcher watcher;
    watcher.setCoalesceMs(30);
    QVector<FileSystemEvent> got;
    connect(&watcher, &FileWatcher::batchReady, this,
            [&got](const QVector<FileSystemEvent> &batch) { got += batch; });
    QVERIFY(watcher.start({dir.path()}));

    const QString to = dir.path() + "/after.txt";
    QVERIFY(QFile::rename(from, to));

    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(got, FileSystemEvent::Renamed, "after.txt"), 4000);

    bool oldPathSeen = false;
    for (const FileSystemEvent &ev : got) {
        if (ev.type == FileSystemEvent::Renamed && ev.oldPath.endsWith("before.txt")) {
            oldPathSeen = true;
            break;
        }
    }
    QVERIFY2(oldPathSeen, "Renamed 事件必须带 oldPath");
}

void TestFileWatcher::modified()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + "/data.txt";
    QVERIFY(writeFile(file, "1"));

    FileWatcher watcher;
    watcher.setCoalesceMs(30);
    QVector<FileSystemEvent> got;
    connect(&watcher, &FileWatcher::batchReady, this,
            [&got](const QVector<FileSystemEvent> &batch) { got += batch; });
    QVERIFY(watcher.start({dir.path()}));

    QFile f(file);
    QVERIFY(f.open(QIODevice::Append));
    f.write("more bytes");
    f.close();

    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(got, FileSystemEvent::Modified, "data.txt")
                                 || hasEvent(got, FileSystemEvent::Added, "data.txt"), 4000);
}

void TestFileWatcher::coalescingDeduplicates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    FileWatcher watcher;
    watcher.setCoalesceMs(80);
    QVector<FileSystemEvent> got;
    connect(&watcher, &FileWatcher::batchReady, this,
            [&got](const QVector<FileSystemEvent> &batch) { got += batch; });
    QVERIFY(watcher.start({dir.path()}));

    for (int i = 0; i < 5; ++i)
        QVERIFY(writeFile(dir.path() + QStringLiteral("/multi%1.txt").arg(i)));

    QTRY_VERIFY_WITH_TIMEOUT(got.size() >= 5, 4000);

    // 同一批里不应出现同一路径两次（合并窗口内的去重）
    QSet<QString> seen;
    for (const FileSystemEvent &ev : got) {
        if (ev.type == FileSystemEvent::Overflow)
            continue;
        const QString key = ev.path.toLower();
        QVERIFY2(!seen.contains(key), qPrintable("duplicate path in batch: " + key));
        seen.insert(key);
    }
}

void TestFileWatcher::dispatcherSubscribePublish()
{
    FileEventDispatcher dispatcher;
    QCOMPARE(dispatcher.subscriberCount(), 0);

    QStringList received;
    const int a = dispatcher.subscribe([&received](const FileSystemEvent &ev) {
        received << ("A:" + ev.path);
    });
    const int b = dispatcher.subscribe([&received](const FileSystemEvent &ev) {
        received << ("B:" + ev.path);
    });
    QVERIFY(a > 0);
    QVERIFY(b > a);
    QCOMPARE(dispatcher.subscriberCount(), 2);

    FileSystemEvent ev;
    ev.type = FileSystemEvent::Added;
    ev.path = "/tmp/x.txt";
    dispatcher.publish(ev);
    QCOMPARE(received.size(), 2);
    QCOMPARE(received.at(0), QStringLiteral("A:/tmp/x.txt"));
    QCOMPARE(received.at(1), QStringLiteral("B:/tmp/x.txt"));

    dispatcher.unsubscribe(a);
    QCOMPARE(dispatcher.subscriberCount(), 1);
    received.clear();
    dispatcher.publish(ev);
    QCOMPARE(received.size(), 1);
    QCOMPARE(received.at(0), QStringLiteral("B:/tmp/x.txt"));

    dispatcher.clear();
    QCOMPARE(dispatcher.subscriberCount(), 0);
}

void TestFileWatcher::dispatcherIsolatesThrowingSubscriber()
{
    FileEventDispatcher dispatcher;
    dispatcher.subscribe([](const FileSystemEvent &) { throw std::runtime_error("boom"); });
    int after = 0;
    dispatcher.subscribe([&after](const FileSystemEvent &) { ++after; });

    FileSystemEvent ev;
    ev.type = FileSystemEvent::Overflow;
    ev.path = "D:/";
    dispatcher.publish(ev);

    QCOMPARE(after, 1);   // 前一个订阅者抛异常不影响后一个
}

QTEST_GUILESS_MAIN(TestFileWatcher)
#include "test_file_watcher.moc"
