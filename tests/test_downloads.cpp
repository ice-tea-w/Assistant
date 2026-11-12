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
#include "core/FileIndex.h"
#include "core/FileKind.h"
#include "core/FileSnapshot.h"
#include "core/FileWatcher.h"
#include "core/FileEventDispatcher.h"
#include "core/QueryParser.h"
#include "core/inbox/DownloadRules.h"
#include "core/inbox/IDownloadSource.h"
#include "core/inbox/IngestEngine.h"
#include "core/inbox/TrackingEngine.h"
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
    void trackingFindsMovedFileByFileId();
    void trackingTimeoutFallsBackToUnknown();
    void trackingIgnoresCopyWhileBoxFileRemains();
    void trackingFollowsSecondMove();
    void offlineReconcileFindsMovedFile();
    void offlineReconcileDetectsBoxRename();
    void rulesMatchKindSizeAndPriority();
    void ingestSuggestsThenArchives();
    void autoOrganizeMovesImmediately();
    void ingestMarksDuplicate();
    void skipRuleLeavesFileInDownloads();

private:
    /// 模拟一条"已在收件箱"的记录：文件真实存在，身份/签名按真实文件采集。
    static int insertInBox(DownloadsDatabase &db, const QString &path)
    {
        FileIdentity::Identity identity;
        const bool ok = FileIdentity::read(path, &identity);
        const QFileInfo info(path);

        DownloadRow row;
        row.sourceId = QStringLiteral("browser");
        row.srcPath = info.absolutePath() + "/" + info.fileName();
        row.boxPath = path;
        row.curPath = path;
        row.inBox = true;
        row.name = info.fileName();
        row.size = info.size();
        row.mtime = info.lastModified().toSecsSinceEpoch();
        row.volumeSerial = ok ? identity.volumeSerial : 0;
        row.fileId = ok ? identity.fileId : QString();
        row.sigQuick = FileIdentity::quickSignature(path);
        row.pipeState = QStringLiteral("InBox");
        row.trackState = QStringLiteral("InBox");
        row.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
        return db.insert(row);
    }

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

void TestDownloads::trackingFindsMovedFileByFileId()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString boxDir = dir.path() + "/Inbox";
    const QString dest = dir.path() + "/Archive";
    QVERIFY(QDir().mkpath(boxDir));
    QVERIFY(QDir().mkpath(dest));

    const QString src = boxDir + "/report.pdf";
    const QString moved = dest + "/report.pdf";
    QVERIFY(writeFile(src, "track me"));

    DownloadsDatabase db(QStringLiteral("downloads_test_track1"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));
    const int id = insertInBox(db, src);
    QVERIFY(id > 0);

    TrackingEngine tracking(&db, nullptr);
    tracking.setBoxPath(boxDir);
    tracking.setWindowMs(3000);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    tracking.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });
    QVERIFY(watcher.start({ boxDir, dest }));
    tracking.start();

    QVERIFY(QFile::rename(src, moved));

    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        const auto row = db.byId(id);
        return row && row->trackState == QStringLiteral("FiledAway") && row->curPath == moved;
    }()), 8000);

    const auto row = db.byId(id);
    QVERIFY(!row->inBox);
    const auto events = db.trackingFor(id);
    QCOMPARE(events.size(), qsizetype(1));
    QCOMPARE(events.first().at(2), QStringLiteral("watcher-frn"));
    QCOMPARE(events.first().at(3), QStringLiteral("exact"));

    watcher.stop();
    tracking.stop();
    db.close();
}

void TestDownloads::trackingTimeoutFallsBackToUnknown()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString boxDir = dir.path() + "/Inbox";
    const QString elsewhere = dir.path() + "/NotWatched";
    QVERIFY(QDir().mkpath(boxDir));
    QVERIFY(QDir().mkpath(elsewhere));

    const QString src = boxDir + "/lost.bin";
    QVERIFY(writeFile(src, "nobody watches the destination"));

    DownloadsDatabase db(QStringLiteral("downloads_test_track2"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));
    const int id = insertInBox(db, src);

    TrackingEngine tracking(&db, nullptr);
    tracking.setBoxPath(boxDir);
    tracking.setWindowMs(500);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    tracking.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });
    QVERIFY(watcher.start({ boxDir }));
    tracking.start();

    QVERIFY(QFile::rename(src, elsewhere + "/lost.bin"));

    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        const auto row = db.byId(id);
        return row && row->trackState == QStringLiteral("Unknown");
    }()), 6000);

    const auto events = db.trackingFor(id);
    QCOMPARE(events.size(), qsizetype(1));
    QCOMPARE(events.first().at(3), QStringLiteral("unknown"));

    watcher.stop();
    tracking.stop();
    db.close();
}

void TestDownloads::trackingIgnoresCopyWhileBoxFileRemains()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString boxDir = dir.path() + "/Inbox";
    const QString dest = dir.path() + "/Archive";
    QVERIFY(QDir().mkpath(boxDir));
    QVERIFY(QDir().mkpath(dest));

    const QString src = boxDir + "/keep.pdf";
    QVERIFY(writeFile(src, "copy, not move"));

    DownloadsDatabase db(QStringLiteral("downloads_test_track3"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));
    const int id = insertInBox(db, src);

    TrackingEngine tracking(&db, nullptr);
    tracking.setBoxPath(boxDir);
    tracking.setWindowMs(800);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    tracking.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });
    QVERIFY(watcher.start({ boxDir, dest }));
    tracking.start();

    QVERIFY(QFile::copy(src, dest + "/keep.pdf"));
    QTest::qWait(1800);

    const auto row = db.byId(id);
    QVERIFY2(row && row->inBox, "别处复制（原件仍在 box）不能触发追踪");
    QCOMPARE(row->trackState, QStringLiteral("InBox"));
    QCOMPARE(db.trackingFor(id).size(), qsizetype(0));

    watcher.stop();
    tracking.stop();
    db.close();
}

void TestDownloads::trackingFollowsSecondMove()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString boxDir = dir.path() + "/Inbox";
    const QString stage1 = dir.path() + "/Stage1";
    const QString stage2 = dir.path() + "/Stage2";
    QVERIFY(QDir().mkpath(boxDir));
    QVERIFY(QDir().mkpath(stage1));
    QVERIFY(QDir().mkpath(stage2));

    const QString src = boxDir + "/chain.pdf";
    QVERIFY(writeFile(src, "multi-hop"));

    DownloadsDatabase db(QStringLiteral("downloads_test_track4"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));
    const int id = insertInBox(db, src);

    TrackingEngine tracking(&db, nullptr);
    tracking.setBoxPath(boxDir);
    tracking.setWindowMs(3000);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    tracking.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });
    QVERIFY(watcher.start({ boxDir, stage1, stage2 }));
    tracking.start();

    const QString p1 = stage1 + "/chain.pdf";
    const QString p2 = stage2 + "/chain.pdf";
    QVERIFY(QFile::rename(src, p1));
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        const auto row = db.byId(id);
        return row && row->curPath == p1 && row->trackState == QStringLiteral("FiledAway");
    }()), 8000);

    QVERIFY(QFile::rename(p1, p2));
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        const auto row = db.byId(id);
        return row && row->curPath == p2 && row->trackState == QStringLiteral("FiledAway");
    }()), 8000);

    QCOMPARE(db.trackingFor(id).size(), qsizetype(2));   // box → Stage1 → Stage2

    watcher.stop();
    tracking.stop();
    db.close();
}

void TestDownloads::offlineReconcileFindsMovedFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString boxDir = dir.path() + "/Inbox";
    const QString archive = dir.path() + "/Archive";
    QVERIFY(QDir().mkpath(boxDir));
    QVERIFY(QDir().mkpath(archive));

    // 关机期间：文件从 box 被移到 Archive，box 里已不存在
    const QString dest = archive + "/moved.pdf";
    QVERIFY(writeFile(dest, "offline moved payload"));
    const QFileInfo info(dest);

    FileIdentity::Identity identity;
    QVERIFY(FileIdentity::read(dest, &identity));

    DownloadsDatabase db(QStringLiteral("downloads_test_offline1"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    DownloadRow row;
    row.sourceId = QStringLiteral("browser");
    row.srcPath = dir.path() + "/Downloads/moved.pdf";
    row.boxPath = boxDir + "/moved.pdf";   // 已不存在
    row.curPath = row.boxPath;
    row.inBox = true;
    row.name = QStringLiteral("moved.pdf");
    row.size = info.size();
    row.mtime = info.lastModified().toSecsSinceEpoch();
    row.volumeSerial = identity.volumeSerial;
    row.fileId = identity.fileId;
    row.sigQuick = FileIdentity::quickSignature(dest);
    row.pipeState = QStringLiteral("InBox");
    row.trackState = QStringLiteral("InBox");
    row.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    const int id = db.insert(row);
    QVERIFY(id > 0);

    // 索引快照里已经有 Archive 的新位置（启动扫描完成后的状态）
    FileIndex index;
    FileSnapshot::Builder builder;
    builder.add(QStringLiteral("moved.pdf"), dest, row.size, row.mtime, FileKind::Pdf);
    index.setSnapshot(builder.finish());

    TrackingEngine tracking(&db, &index);
    tracking.setBoxPath(boxDir);
    tracking.reconcileOffline();

    const auto fixed = db.byId(id);
    QVERIFY(fixed.has_value());
    QVERIFY(!fixed->inBox);
    QCOMPARE(fixed->trackState, QStringLiteral("FiledAway"));
    QCOMPARE(fixed->curPath, dest);

    const auto events = db.trackingFor(id);
    QCOMPARE(events.size(), qsizetype(1));
    QCOMPARE(events.first().at(2), QStringLiteral("startup-rescan"));
    QCOMPARE(events.first().at(3), QStringLiteral("strong"));

    db.close();
}

void TestDownloads::offlineReconcileDetectsBoxRename()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString boxDir = dir.path() + "/Inbox";
    QVERIFY(QDir().mkpath(boxDir));

    // 关机期间在 box 内改名
    const QString renamed = boxDir + "/new-name.txt";
    QVERIFY(writeFile(renamed, "renamed while app closed"));
    const QFileInfo info(renamed);

    FileIdentity::Identity identity;
    QVERIFY(FileIdentity::read(renamed, &identity));

    DownloadsDatabase db(QStringLiteral("downloads_test_offline2"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    DownloadRow row;
    row.sourceId = QStringLiteral("browser");
    row.srcPath = dir.path() + "/Downloads/old-name.txt";
    row.boxPath = boxDir + "/old-name.txt";   // 旧名字，已不存在
    row.curPath = row.boxPath;
    row.inBox = true;
    row.name = QStringLiteral("old-name.txt");
    row.size = info.size();
    row.mtime = info.lastModified().toSecsSinceEpoch();
    row.volumeSerial = identity.volumeSerial;
    row.fileId = identity.fileId;
    row.sigQuick = FileIdentity::quickSignature(renamed);
    row.pipeState = QStringLiteral("InBox");
    row.trackState = QStringLiteral("InBox");
    row.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    const int id = db.insert(row);
    QVERIFY(id > 0);

    FileIndex index;   // 级别 0 只看 box，不需要索引内容，但需要 isReady
    FileSnapshot::Builder empty;
    index.setSnapshot(empty.finish());

    TrackingEngine tracking(&db, &index);
    tracking.setBoxPath(boxDir);
    tracking.reconcileOffline();

    const auto fixed = db.byId(id);
    QVERIFY(fixed.has_value());
    QVERIFY2(fixed->inBox, "box 内改名不应变成已归档");
    QCOMPARE(fixed->boxPath, renamed);
    QCOMPARE(fixed->curPath, renamed);

    db.close();
}

void TestDownloads::rulesMatchKindSizeAndPriority()
{
    QVariantList config;
    auto addRule = [&config](const QString &name, int priority, const QString &kind,
                             const QString &size, const QString &action, const QString &dest) {
        QVariantMap rule;
        rule.insert(QStringLiteral("name"), name);
        rule.insert(QStringLiteral("priority"), priority);
        rule.insert(QStringLiteral("kind"), kind);
        rule.insert(QStringLiteral("size"), size);
        rule.insert(QStringLiteral("action"), action);
        rule.insert(QStringLiteral("dest"), dest);
        config.append(rule);
    };

    addRule(QStringLiteral("pdf-first"), 10, QStringLiteral("pdf"), QString(),
            QStringLiteral("move"), QStringLiteral("/dest/PDF/{yyyy}"));
    addRule(QStringLiteral("docs"), 20, QStringLiteral("document,sheet"), QString(),
            QStringLiteral("suggest"), QStringLiteral("/dest/Docs/{name}"));
    addRule(QStringLiteral("big"), 5, QString(), QStringLiteral(">100mb"),
            QStringLiteral("notify"), QString());

    DownloadRules rules;
    rules.load(config);
    QCOMPARE(rules.count(), 3);

    DownloadRow pdf;
    pdf.name = QStringLiteral("report.pdf");
    pdf.ext = QStringLiteral("pdf");
    pdf.catMask = FileKind::Pdf;
    pdf.size = 1024;
    const auto pdfDecision = rules.evaluate(pdf, QStringLiteral("/inbox"));
    QCOMPARE(pdfDecision.action, QStringLiteral("move"));
    QCOMPARE(pdfDecision.ruleName, QStringLiteral("pdf-first"));
    QVERIFY(pdfDecision.dest.endsWith(QStringLiteral("/dest/PDF/")
                                      + QString::number(QDate::currentDate().year())));

    DownloadRow doc;
    doc.name = QStringLiteral("notes.txt");
    doc.ext = QStringLiteral("txt");
    doc.catMask = FileKind::Document;
    doc.size = 100;
    const auto docDecision = rules.evaluate(doc, QStringLiteral("/inbox"));
    QCOMPARE(docDecision.action, QStringLiteral("suggest"));
    QCOMPARE(docDecision.ruleName, QStringLiteral("docs"));
    QCOMPARE(docDecision.dest, QStringLiteral("/dest/Docs/notes.txt"));

    doc.size = 200LL * 1024 * 1024;   // 命中优先级最高的 big (>100mb)
    const auto bigDecision = rules.evaluate(doc, QStringLiteral("/inbox"));
    QCOMPARE(bigDecision.ruleName, QStringLiteral("big"));
    QCOMPARE(bigDecision.action, QStringLiteral("notify"));
}

void TestDownloads::ingestSuggestsThenArchives()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString downloadsDir = dir.path() + "/Downloads";
    const QString boxDir = dir.path() + "/Inbox";
    const QString archiveDir = dir.path() + "/ArchivePDF";
    QVERIFY(QDir().mkpath(downloadsDir));

    DownloadsDatabase db(QStringLiteral("downloads_test_rules1"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    BrowserSource source(downloadsDir);
    IngestEngine engine(&db);
    engine.setSource(&source);
    engine.setBoxPath(boxDir);
    engine.setSettleMs(120);
    engine.setEnabled(true);

    QVariantList config;
    QVariantMap rule;
    rule.insert(QStringLiteral("name"), QStringLiteral("pdf"));
    rule.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    rule.insert(QStringLiteral("action"), QStringLiteral("suggest"));
    rule.insert(QStringLiteral("dest"), archiveDir);
    config.append(rule);
    DownloadRules rules;
    rules.load(config);
    engine.setRules(rules);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    engine.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });

    QSignalSpy captured(&engine, &IngestEngine::captured);
    QVERIFY(engine.start());
    QVERIFY(watcher.start({ downloadsDir }));

    QVERIFY(writeFile(downloadsDir + "/paper.pdf", "suggest me"));
    QTRY_VERIFY_WITH_TIMEOUT(captured.count() > 0, 10000);

    const int id = captured.first().at(0).toInt();
    auto row = db.byId(id);
    QVERIFY(row.has_value());
    QVERIFY(row->inBox);
    QCOMPARE(row->suggestDest, archiveDir);
    QVERIFY(QFileInfo::exists(boxDir + "/paper.pdf"));

    QVERIFY(engine.archive(id, row->suggestDest, nullptr));
    QVERIFY(QFileInfo::exists(archiveDir + "/paper.pdf"));
    QVERIFY(!QFileInfo::exists(boxDir + "/paper.pdf"));
    row = db.byId(id);
    QVERIFY(!row->inBox);
    QCOMPARE(row->trackState, QStringLiteral("FiledAway"));

    watcher.stop();
    engine.setEventSource(nullptr);
    engine.stop();
    db.close();
}

void TestDownloads::autoOrganizeMovesImmediately()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString downloadsDir = dir.path() + "/Downloads";
    const QString boxDir = dir.path() + "/Inbox";
    const QString archiveDir = dir.path() + "/AutoArchive";
    QVERIFY(QDir().mkpath(downloadsDir));

    DownloadsDatabase db(QStringLiteral("downloads_test_auto"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    BrowserSource source(downloadsDir);
    IngestEngine engine(&db);
    engine.setSource(&source);
    engine.setBoxPath(boxDir);
    engine.setSettleMs(120);
    engine.setEnabled(true);
    engine.setAutoOrganize(true);

    QVariantList config;
    QVariantMap rule;
    rule.insert(QStringLiteral("name"), QStringLiteral("auto-images"));
    rule.insert(QStringLiteral("kind"), QStringLiteral("img"));
    rule.insert(QStringLiteral("action"), QStringLiteral("move"));
    rule.insert(QStringLiteral("dest"), archiveDir);
    config.append(rule);
    DownloadRules rules;
    rules.load(config);
    engine.setRules(rules);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    engine.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });

    QSignalSpy captured(&engine, &IngestEngine::captured);
    QVERIFY(engine.start());
    QVERIFY(watcher.start({ downloadsDir }));

    QVERIFY(writeFile(downloadsDir + "/photo.png", "image bytes"));
    QTRY_VERIFY_WITH_TIMEOUT(captured.count() > 0, 10000);

    const int id = captured.first().at(0).toInt();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(archiveDir + "/photo.png"), 5000);
    QVERIFY(!QFileInfo::exists(boxDir + "/photo.png"));
    const auto row = db.byId(id);
    QVERIFY(row.has_value());
    QVERIFY(!row->inBox);
    QCOMPARE(row->curPath, archiveDir + "/photo.png");

    watcher.stop();
    engine.setEventSource(nullptr);
    engine.stop();
    db.close();
}

void TestDownloads::ingestMarksDuplicate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString downloadsDir = dir.path() + "/Downloads";
    const QString boxDir = dir.path() + "/Inbox";
    QVERIFY(QDir().mkpath(downloadsDir));

    DownloadsDatabase db(QStringLiteral("downloads_test_dup2"));
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

    QVERIFY(writeFile(downloadsDir + "/same-a.txt", "identical content"));
    QTRY_VERIFY_WITH_TIMEOUT(captured.count() >= 1, 10000);
    const int firstId = captured.at(0).at(0).toInt();

    QVERIFY(writeFile(downloadsDir + "/same-b.txt", "identical content"));
    QTRY_VERIFY_WITH_TIMEOUT(captured.count() >= 2, 10000);
    const int secondId = captured.at(1).at(0).toInt();

    const auto second = db.byId(secondId);
    QVERIFY(second.has_value());
    QCOMPARE(second->dupOf, firstId);

    watcher.stop();
    engine.setEventSource(nullptr);
    engine.stop();
    db.close();
}

void TestDownloads::skipRuleLeavesFileInDownloads()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString downloadsDir = dir.path() + "/Downloads";
    const QString boxDir = dir.path() + "/Inbox";
    QVERIFY(QDir().mkpath(downloadsDir));

    DownloadsDatabase db(QStringLiteral("downloads_test_skip"));
    QVERIFY(db.open(dir.path() + "/downloads.db"));

    BrowserSource source(downloadsDir);
    IngestEngine engine(&db);
    engine.setSource(&source);
    engine.setBoxPath(boxDir);
    engine.setSettleMs(120);
    engine.setEnabled(true);

    QVariantList config;
    QVariantMap rule;
    rule.insert(QStringLiteral("name"), QStringLiteral("no-installers"));
    rule.insert(QStringLiteral("kind"), QStringLiteral("exe"));
    rule.insert(QStringLiteral("action"), QStringLiteral("skip"));
    config.append(rule);
    DownloadRules rules;
    rules.load(config);
    engine.setRules(rules);

    FileEventDispatcher dispatcher;
    FileWatcher watcher;
    watcher.setCoalesceMs(50);
    engine.setEventSource(&dispatcher);
    connect(&watcher, &FileWatcher::batchReady, this,
            [&dispatcher](const QVector<FileSystemEvent> &batch) { dispatcher.publish(batch); });

    QSignalSpy captured(&engine, &IngestEngine::captured);
    QVERIFY(engine.start());
    QVERIFY(watcher.start({ downloadsDir }));

    const QString exe = downloadsDir + "/setup.exe";
    QVERIFY(writeFile(exe, "installer payload"));
    QTest::qWait(1500);

    QCOMPARE(captured.count(), 0);
    QVERIFY(QFileInfo::exists(exe));       // 源文件留在 Downloads
    QVERIFY(!QFileInfo::exists(boxDir + "/setup.exe"));

    watcher.stop();
    engine.setEventSource(nullptr);
    engine.stop();
    db.close();
}

QTEST_GUILESS_MAIN(TestDownloads)
#include "test_downloads.moc"
