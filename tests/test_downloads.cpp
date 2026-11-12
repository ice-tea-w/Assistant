// Phase C（S9 Download Box 骨架）的单元与端到端用例。
//
// 覆盖：
//  - FileIdentity：物理身份（同卷改名不变）+ quickSig（内容签名）
//  - DownloadsDatabase：双状态字段 / move_journal / app_meta / sources
//  - QueryParser：`:dl` 与 `in:box`
//  - IngestEngine：发现 → 稳定判定 → 拉入 box → 登记 → 撤销；同名冲突；启动对账

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "core/FileIdentity.h"
#include "core/FileKind.h"
#include "core/FileWatcher.h"
#include "core/FileEventDispatcher.h"
#include "core/QueryParser.h"
#include "core/inbox/IDownloadSource.h"
#include "core/inbox/IngestEngine.h"
#include "database/DownloadsDatabase.h"

class TestDownloads : public QObject
{
    Q_OBJECT

private slots:
    void identitySurvivesRename();
    void quickSignatureTracksContent();

    void databaseRoundTrip();
    void parserMapsDownloadTokens();

    void ingestMovesCompletedDownloadToBox();
    void partialDownloadIsNotTaken();
    void nameCollisionGetsSuffix();
    void reconcileMarksMissingBoxFileUnknown();

private:
    static bool writeFile(const QString &path, const QByteArray &content = "payload")
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(content);
        f.close();
        return true;
    }
};

void TestDownloads::identitySurvivesRename()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString a = dir.path() + "/a.txt";
    const QString b = dir.path() + "/b.txt";
    QVERIFY(writeFile(a, "hello identity"));

    FileIdentity::Identity first;
    QVERIFY(FileIdentity::read(a, &first));
    QVERIFY(first.size > 0);
    QVERIFY(first.volumeSerial != 0);
    QCOMPARE(first.fileId.size(), qsizetype(32));
    const QString sig = FileIdentity::quickSignature(a);
    QCOMPARE(sig.size(), qsizetype(64));

    QVERIFY(QFile::rename(a, b));
    FileIdentity::Identity second;
    QVERIFY(FileIdentity::read(b, &second));
    QCOMPARE(second.fileId, first.fileId);      // NTFS 同卷改名 FRN 不变（路径 A 的依据）
    QCOMPARE(second.volumeSerial, first.volumeSerial);
    QCOMPARE(FileIdentity::quickSignature(b), sig);
}

void TestDownloads::quickSignatureTracksContent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QByteArray big(200 * 1024, 'a');
    const QString path = dir.path() + "/big.bin";
    QVERIFY(writeFile(path, big));
    const QString sig1 = FileIdentity::quickSignature(path);
    QCOMPARE(sig1.size(), qsizetype(64));

    // 改文件头 → 签名必须变化
    big[0] = 'b';
    QVERIFY(writeFile(path, big));
    const QString sig2 = FileIdentity::quickSignature(path);
    QVERIFY(sig1 != sig2);

    // 只改中间（不在首尾 64KB）也可能不变，这是 quickSig 的概率性语义 —— 这里只验证稳定
    QCOMPARE(FileIdentity::quickSignature(path), sig2);
}

void TestDownloads::databaseRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    DownloadsDatabase db(QStringLiteral("downloads_test_rt"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));
    QVERIFY(db.isOpen());

    DownloadRow row;
    row.sourceId = QStringLiteral("browser");
    row.srcPath = dir.path() + "/Downloads/report.pdf";
    row.name = QStringLiteral("report.pdf");
    row.ext = QStringLiteral("pdf");
    row.catMask = FileKind::Pdf;
    row.size = 123;
    row.mtime = 1700000000;
    row.pipeState = QStringLiteral("Fetching");
    row.trackState = QStringLiteral("InBox");

    const int id = db.insert(row);
    QVERIFY(id > 0);

    QVERIFY(db.updateAfterFetch(id, dir.path() + "/Inbox/report.pdf", 123, 1700000000,
                                42, QString(32, QLatin1Char('a')), QString(64, QLatin1Char('b'))));

    const auto loaded = db.byId(id);
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->inBox);
    QCOMPARE(loaded->pipeState, QStringLiteral("InBox"));
    QCOMPARE(loaded->trackState, QStringLiteral("InBox"));
    QCOMPARE(loaded->sigQuick.size(), qsizetype(64));
    QCOMPARE(db.countInBox(), 1);
    QCOMPARE(db.listInBox().size(), qsizetype(1));

    const qint64 journal = db.beginJournal(id, QStringLiteral("fetch"),
                                           row.srcPath, loaded->boxPath);
    QVERIFY(journal > 0);
    QCOMPARE(db.pendingIntents().size(), qsizetype(1));
    QVERIFY(db.finishJournal(journal, true));
    QVERIFY(db.pendingIntents().isEmpty());
    QCOMPARE(db.journalFor(id).size(), qsizetype(1));

    QVERIFY(db.setMeta(QStringLiteral("last_exit_at"), QStringLiteral("2026-11-12T10:00:00")));
    QCOMPARE(db.meta(QStringLiteral("last_exit_at")), QStringLiteral("2026-11-12T10:00:00"));

    DownloadsDatabase::SourceRow source;
    source.sourceId = QStringLiteral("browser");
    source.enabled = true;
    source.pathsJson = QStringLiteral("[]");
    QVERIFY(db.upsertSource(source));
    const auto back = db.source(QStringLiteral("browser"));
    QVERIFY(back.has_value());
    QVERIFY(back->enabled);

    // 撤销：pipe=Undone / in_box=0
    QVERIFY(db.updatePipeState(id, QStringLiteral("Undone"), 0));
    QVERIFY(db.updateTrackState(id, QStringLiteral("Unknown"), row.srcPath, false));
    QCOMPARE(db.countInBox(), 0);

    db.close();
}

void TestDownloads::parserMapsDownloadTokens()
{
    const QueryParser::Parsed dl = QueryParser::parse(QStringLiteral(":dl report"));
    QCOMPARE(dl.inFilters, QStringList({ QStringLiteral("inbox") }));
    QCOMPARE(dl.terms, QStringList({ QStringLiteral("report") }));

    const QueryParser::Parsed box = QueryParser::parse(QStringLiteral("in:box"));
    QCOMPARE(box.inFilters, QStringList({ QStringLiteral("inbox") }));

    const QueryParser::Parsed both = QueryParser::parse(QStringLiteral(":dl in:box"));
    QCOMPARE(both.inFilters.size(), qsizetype(1));   // 去重
}

void TestDownloads::ingestMovesCompletedDownloadToBox()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString downloadsDir = dir.path() + "/Downloads";
    const QString boxDir = dir.path() + "/Inbox";
    QVERIFY(QDir().mkpath(downloadsDir));

    DownloadsDatabase db(QStringLiteral("downloads_test_e2e"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    BrowserSource source(downloadsDir);
    IngestEngine engine(&db);
    engine.setSource(&source);
    engine.setBoxPath(boxDir);
    engine.setSettleMs(120);
    engine.setEnabled(true);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    engine.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });

    QSignalSpy captured(&engine, &IngestEngine::captured);
    QVERIFY(engine.start());
    QVERIFY(watcher.start({ downloadsDir }));

    const QString src = downloadsDir + "/report.pdf";
    QVERIFY(writeFile(src, "pdf payload"));

    QTRY_VERIFY_WITH_TIMEOUT(captured.count() > 0, 10000);
    QVERIFY(!QFileInfo::exists(src));
    QVERIFY(QFileInfo::exists(boxDir + "/report.pdf"));

    const int id = captured.first().at(0).toInt();
    const auto row = db.byId(id);
    QVERIFY(row.has_value());
    QVERIFY(row->inBox);
    QCOMPARE(row->pipeState, QStringLiteral("InBox"));
    QCOMPARE(row->name, QStringLiteral("report.pdf"));
    QCOMPARE(row->sigQuick.size(), qsizetype(64));
    QCOMPARE(row->fileId.size(), qsizetype(32));
    QVERIFY(row->mtime > 0);

    // 撤销 → 回到下载目录
    QVERIFY(engine.undo(id, nullptr));
    QVERIFY(QFileInfo::exists(src));
    QVERIFY(!QFileInfo::exists(boxDir + "/report.pdf"));
    const auto undone = db.byId(id);
    QVERIFY(undone.has_value());
    QVERIFY(!undone->inBox);
    QCOMPARE(undone->pipeState, QStringLiteral("Undone"));

    watcher.stop();
    engine.setEventSource(nullptr);
    engine.stop();
    db.close();
}

void TestDownloads::partialDownloadIsNotTaken()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString downloadsDir = dir.path() + "/Downloads";
    const QString boxDir = dir.path() + "/Inbox";
    QVERIFY(QDir().mkpath(downloadsDir));

    DownloadsDatabase db(QStringLiteral("downloads_test_partial"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    BrowserSource source(downloadsDir);
    IngestEngine engine(&db);
    engine.setSource(&source);
    engine.setBoxPath(boxDir);
    engine.setSettleMs(120);
    engine.setEnabled(true);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    engine.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });

    QSignalSpy captured(&engine, &IngestEngine::captured);
    QVERIFY(engine.start());
    QVERIFY(watcher.start({ downloadsDir }));

    // 浏览器临时名：即使大小稳定也不能拉（条件 A 不满足）
    const QString partial = downloadsDir + "/video.mp4.crdownload";
    QVERIFY(writeFile(partial, "partial data"));
    QTest::qWait(1200);
    QCOMPARE(captured.count(), 0);
    QVERIFY(QFileInfo::exists(partial));
    QVERIFY(!QFileInfo::exists(boxDir + "/video.mp4"));

    // 浏览器完成后重命名为最终名 → 才进入候选并接管
    const QString finalPath = downloadsDir + "/video.mp4";
    QVERIFY(QFile::rename(partial, finalPath));
    QTRY_VERIFY_WITH_TIMEOUT(captured.count() > 0, 10000);
    QVERIFY(QFileInfo::exists(boxDir + "/video.mp4"));
    QVERIFY(!QFileInfo::exists(finalPath));

    watcher.stop();
    engine.setEventSource(nullptr);
    engine.stop();
    db.close();
}

void TestDownloads::nameCollisionGetsSuffix()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString downloadsDir = dir.path() + "/Downloads";
    const QString boxDir = dir.path() + "/Inbox";
    QVERIFY(QDir().mkpath(downloadsDir));
    QVERIFY(QDir().mkpath(boxDir));
    QVERIFY(writeFile(boxDir + "/report.pdf", "old"));

    DownloadsDatabase db(QStringLiteral("downloads_test_dup"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    BrowserSource source(downloadsDir);
    IngestEngine engine(&db);
    engine.setSource(&source);
    engine.setBoxPath(boxDir);
    engine.setSettleMs(120);
    engine.setEnabled(true);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    engine.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });

    QSignalSpy captured(&engine, &IngestEngine::captured);
    QVERIFY(engine.start());
    QVERIFY(watcher.start({ downloadsDir }));

    QVERIFY(writeFile(downloadsDir + "/report.pdf", "new"));
    QTRY_VERIFY_WITH_TIMEOUT(captured.count() > 0, 10000);

    QVERIFY(QFileInfo::exists(boxDir + "/report.pdf"));
    QVERIFY2(QFileInfo::exists(boxDir + "/report (1).pdf"), "同名必须生成 (1) 副本，不覆盖");

    watcher.stop();
    engine.setEventSource(nullptr);
    engine.stop();
    db.close();
}

void TestDownloads::reconcileMarksMissingBoxFileUnknown()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString boxDir = dir.path() + "/Inbox";
    QVERIFY(QDir().mkpath(boxDir));

    DownloadsDatabase db(QStringLiteral("downloads_test_reconcile"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    DownloadRow row;
    row.sourceId = QStringLiteral("browser");
    row.srcPath = dir.path() + "/Downloads/gone.pdf";
    row.boxPath = boxDir + "/gone.pdf";
    row.curPath = row.boxPath;
    row.inBox = true;
    row.name = QStringLiteral("gone.pdf");
    row.size = 10;
    row.mtime = 1700000000;
    row.pipeState = QStringLiteral("InBox");
    row.trackState = QStringLiteral("InBox");
    const int id = db.insert(row);
    QVERIFY(id > 0);

    BrowserSource source(dir.path() + "/Downloads");
    IngestEngine engine(&db);
    engine.setSource(&source);
    engine.setBoxPath(boxDir);
    QVERIFY(engine.start());   // 启动对账不应要求 enabled

    const auto fixed = db.byId(id);
    QVERIFY(fixed.has_value());
    QVERIFY(!fixed->inBox);
    QCOMPARE(fixed->trackState, QStringLiteral("Unknown"));

    engine.stop();
    db.close();
}

QTEST_GUILESS_MAIN(TestDownloads)
#include "test_downloads.moc"
