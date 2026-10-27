// 设备同步模块单元测试（QTest 框架）
//
// 只依赖纯逻辑与本地文件系统，不依赖网络、不依赖 vcpkg 之外的东西。
// 不覆盖：SyncEngine（需要真实通道与线程）、SyncBridge（需要 QML）。
//
// 运行：build\Debug\assistant_tests.exe
//   或 ctest --test-dir build -C Debug --output-on-failure

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "core/sync/ConflictResolver.h"
#include "core/sync/DeliveryNaming.h"
#include "core/sync/FolderTransport.h"
#include "core/sync/PathGuard.h"
#include "core/sync/SyncEngine.h"
#include "database/SyncDatabase.h"

#include <memory>

using namespace Sync;

namespace {

/// 把 "大小:时间:哈希" 或 "n"（不存在）解析成 FileState，便于表驱动测试。
FileState stateFrom(const QString &spec)
{
    FileState s;
    if (spec == QLatin1String("n"))
        return s;
    const QStringList p = spec.split(QLatin1Char(':'));
    s.size  = p.value(0).toLongLong();
    s.mtime = p.value(1).toLongLong();
    s.hash  = p.value(2);
    return s;
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

} // namespace

class TestSync : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void sameStateRules();
    void decideTable_data();
    void decideTable();
    void decideDanglingStates();
    void conflictNameFormat();

    void pathGuard_data();
    void pathGuard();
    void resolveUnderStaysInsideRoot();

    void databaseBaseline();
    void databaseConflicts();

    void folderTransportRoundTrip();
    void folderTransportRejectsTraversal();
    void folderTransportFetchMissing();
    void folderTransportLockHeld();
    void folderTransportLockTakeover();
    void folderTransportRemoteAliases();

    void engineTwoDevicesSequential();
    void engineTwoRootsNoCollision();
    void engineSkipsWhenRemoteEmpty();

    void configRootsRoundTrip();

    void deliveryNameRoundTrip();
    void deliveryOwnCheck();
    void deliveryPlaceIncoming();
    void databaseReceived();
    void configWatchInboxRoundTrip();
};

void TestSync::initTestCase()
{
    const QString logDir = QDir::tempPath() + "/assistant-test-logs";
    QDir().mkpath(logDir);
    Logger::init(logDir.toStdString(), "warn");
}

// ---------------------------------------------------------------- 判定逻辑

void TestSync::sameStateRules()
{
    // 双方都有 hash → 只比 hash（大小与时间不同也算相同）
    FileState a{5, 100, "h1"};
    FileState b{9, 999, "h1"};
    QVERIFY(sameState(a, b));

    // 任一方缺 hash → 退化比 (size, mtime)
    FileState c{5, 100, ""};
    FileState d{5, 100, ""};
    QVERIFY(sameState(c, d));
    QVERIFY(!sameState(c, FileState{5, 101, ""}));
    QVERIFY(!sameState(c, FileState{6, 100, ""}));

    // hash 一侧为空字符串时同样走退化分支
    QVERIFY(sameState(FileState{5, 100, "h1"}, FileState{5, 100, ""}));

    // 不存在与存在永不相等
    FileState none;
    QVERIFY(!sameState(none, a));
    QVERIFY(sameState(none, FileState{}));
}

void TestSync::decideTable_data()
{
    QTest::addColumn<QString>("L");
    QTest::addColumn<QString>("S");
    QTest::addColumn<QString>("R");
    QTest::addColumn<int>("expected");

    const int None_ = int(SyncAction::None);
    const int Up    = int(SyncAction::Upload);
    const int Down  = int(SyncAction::Download);
    const int Base  = int(SyncAction::UpdateBaseline);
    const int Conf  = int(SyncAction::Conflict);
    const int DelL  = int(SyncAction::DeleteLocal);
    const int DelR  = int(SyncAction::DeleteRemote);

    QTest::newRow("1 三方一致 → 无动作")
        << "5:100:h1" << "5:100:h1" << "5:100:h1" << None_;
    QTest::newRow("2 本地新增 → 上传")
        << "5:100:h1" << "n"        << "n"        << Up;
    QTest::newRow("3 本地修改 → 上传")
        << "5:200:h2" << "5:100:h1" << "5:100:h1" << Up;
    QTest::newRow("4 本地删除（对端未改）→ 请求对端删除")
        << "n"        << "5:100:h1" << "5:100:h1" << DelR;
    QTest::newRow("5 对端新增 → 下载")
        << "n"        << "n"        << "5:100:h1" << Down;
    QTest::newRow("6 对端修改 → 下载")
        << "5:100:h1" << "5:100:h1" << "5:200:h2" << Down;
    QTest::newRow("7 对端删除（本地未改）→ 本地删除")
        << "5:100:h1" << "5:100:h1" << "n"        << DelL;
    QTest::newRow("8 两端改成同一内容 → 只更新基线")
        << "5:200:h2" << "5:100:h1" << "5:200:h2" << Base;
    QTest::newRow("9 真冲突：两端改成不同内容")
        << "5:200:h2" << "5:100:h1" << "5:300:h3" << Conf;
    QTest::newRow("10 两端都删除 → 只更新基线")
        << "n"        << "5:100:h1" << "n"        << Base;
}

void TestSync::decideTable()
{
    QFETCH(QString, L);
    QFETCH(QString, S);
    QFETCH(QString, R);
    QFETCH(int, expected);

    const SyncAction actual = decide(stateFrom(L), stateFrom(S), stateFrom(R));
    QCOMPARE(int(actual), expected);
}

void TestSync::decideDanglingStates()
{
    // 基线为空（首次同步）时的两侧组合，不能推出"删除"
    const FileState none;

    // 本地有、基线空、对端空 → 上传
    QCOMPARE(int(decide(FileState{5, 100, "h1"}, none, none)), int(SyncAction::Upload));

    // 本地空、基线空、对端有 → 下载
    QCOMPARE(int(decide(none, none, FileState{5, 100, "h1"})), int(SyncAction::Download));

    // 两端都有但内容不同、基线空 → 冲突
    QCOMPARE(int(decide(FileState{5, 100, "h1"}, none, FileState{5, 100, "h2"})),
             int(SyncAction::Conflict));

    // 两端都有且内容相同、基线空 → 只更新基线
    QCOMPARE(int(decide(FileState{5, 100, "h1"}, none, FileState{5, 100, "h1"})),
             int(SyncAction::UpdateBaseline));
}

void TestSync::conflictNameFormat()
{
    // 2026-09-24 15:30:00 UTC = 1758727800
    const QString name = makeConflictName("报告.docx", "笔记本", 1758727800);
    QVERIFY2(name.contains(QStringLiteral("报告")), qPrintable(name));
    QVERIFY2(name.contains(QStringLiteral("笔记本")), qPrintable(name));
    QVERIFY2(name.endsWith(QStringLiteral(".docx")), qPrintable(name));

    // 无扩展名的文件不能丢掉整名
    const QString noExt = makeConflictName("README", "台式机", 1758727800);
    QVERIFY2(noExt.contains(QStringLiteral("README")), qPrintable(noExt));

    // 副本名本身仍必须是安全相对路径
    QVERIFY(PathGuard::isSafeRelativePath(name));
}

// ---------------------------------------------------------------- 路径安全

void TestSync::pathGuard_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("safe");

    QTest::newRow("普通文件")     << QStringLiteral("a.txt")                  << true;
    QTest::newRow("子目录")       << QStringLiteral("dir/a.txt")              << true;
    QTest::newRow("中文与空格")   << QStringLiteral("中文/文件 名.txt")        << true;
    QTest::newRow("多段扩展名")   << QStringLiteral("a.b/c.d")                << true;

    QTest::newRow("空路径")       << QString()                                << false;
    QTest::newRow("Unix 绝对")    << QStringLiteral("/etc/passwd")            << false;
    QTest::newRow("盘符绝对")     << QStringLiteral("C:/Windows/system.dll")  << false;
    QTest::newRow("驱动器相对")   << QStringLiteral("C:foo")                  << false;
    QTest::newRow("UNC 正斜杠")   << QStringLiteral("//server/share/x")       << false;
    QTest::newRow("UNC 反斜杠")   << QStringLiteral("\\\\server\\share\\x")   << false;
    QTest::newRow("父目录段")     << QStringLiteral("../x.txt")               << false;
    QTest::newRow("中段父目录")   << QStringLiteral("a/../b.txt")             << false;
    QTest::newRow("当前目录段")   << QStringLiteral("a/./b.txt")              << false;
    QTest::newRow("保留名 AUX")   << QStringLiteral("aux.txt")                << false;
    QTest::newRow("保留名 CON")   << QStringLiteral("CON")                    << false;
    QTest::newRow("NTFS 数据流")  << QStringLiteral("a/b:c")                  << false;
    QTest::newRow("控制字符")     << QStringLiteral("a/b\x01c")               << false;
    QTest::newRow("末尾空格")     << QStringLiteral("a/b ")                   << false;
    QTest::newRow("末尾点")       << QStringLiteral("a/b.")                   << false;
}

void TestSync::pathGuard()
{
    QFETCH(QString, path);
    QFETCH(bool, safe);

    QCOMPARE(PathGuard::isSafeRelativePath(path), safe);
}

void TestSync::resolveUnderStaysInsideRoot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir().mkpath(root.path() + "/sub");
    writeFile(root.path() + "/sub/a.txt", "x");

    const QString ok = PathGuard::resolveUnder(root.path(), "sub/a.txt");
    QVERIFY(!ok.isEmpty());
    QVERIFY(ok.startsWith(QFileInfo(root.path()).canonicalFilePath(), Qt::CaseInsensitive));

    // 不安全路径直接拒绝
    QVERIFY(PathGuard::resolveUnder(root.path(), "../escape.txt").isEmpty());
    QVERIFY(PathGuard::resolveUnder(root.path(), "C:/Windows/x.dll").isEmpty());

    // 父目录不存在也无法解析（宁缺毋滥）
    QVERIFY(PathGuard::resolveUnder(root.path(), "nodir/a.txt").isEmpty());
}

// ---------------------------------------------------------------- 基线存储

void TestSync::databaseBaseline()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SyncDatabase db(QStringLiteral("test_baseline"));
    QVERIFY(db.open(dir.path() + "/sync.db"));
    QVERIFY(db.isOpen());
    QVERIFY(QFile::exists(dir.path() + "/sync.db"));

    FileState s;
    s.size = 100;
    s.mtime = 1700000000;
    s.hash = QStringLiteral("aabb");
    QVERIFY(db.upsertBaseline("notes/a.txt", s, 1700000001, "push"));

    const auto got = db.baseline("notes/a.txt");
    QVERIFY(got.has_value());
    QCOMPARE(got->size, qint64(100));
    QCOMPARE(got->hash, QStringLiteral("aabb"));

    QVERIFY(!db.baseline("nope.txt").has_value());
    QCOMPARE(db.allBaselinePaths(), QStringList{QStringLiteral("notes/a.txt")});

    // 同一路径再次写入 → 覆盖而不是新增
    s.hash = QStringLiteral("ccdd");
    QVERIFY(db.upsertBaseline("notes/a.txt", s, 1700000002, "pull"));
    QCOMPARE(db.allBaselinePaths().size(), 1);
    QCOMPARE(db.baseline("notes/a.txt")->hash, QStringLiteral("ccdd"));

    QVERIFY(db.removeBaseline("notes/a.txt"));
    QVERIFY(!db.baseline("notes/a.txt").has_value());
    QVERIFY(db.allBaselinePaths().isEmpty());

    db.close();
    QVERIFY(!db.isOpen());
}

void TestSync::databaseConflicts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SyncDatabase db(QStringLiteral("test_conflicts"));
    QVERIFY(db.open(dir.path() + "/sync.db"));

    ConflictRecord rec;
    rec.relPath      = QStringLiteral("报告.docx");
    rec.localHash    = QStringLiteral("aaaa");
    rec.remoteHash   = QStringLiteral("bbbb");
    rec.conflictCopy = QStringLiteral("报告 (冲突副本 来自笔记本 20261015-120000).docx");
    rec.detectedAt   = 1760000000;
    QVERIFY(db.insertConflict(rec));

    QList<ConflictRecord> list = db.recentConflicts(10);
    QCOMPARE(list.size(), 1);
    QVERIFY(list.first().id > 0);
    QCOMPARE(list.first().relPath, QStringLiteral("报告.docx"));
    QCOMPARE(list.first().resolved, false);

    QVERIFY(db.markResolved(list.first().id));
    list = db.recentConflicts(10);
    QCOMPARE(list.first().resolved, true);

    db.close();
}

// ---------------------------------------------------------------- 共享目录通道

void TestSync::folderTransportRoundTrip()
{
    QTemporaryDir local, shared, other;
    QVERIFY(local.isValid() && shared.isValid() && other.isValid());

    const QString src = local.path() + "/报告.txt";
    writeFile(src, "hello sync");

    FolderTransport t(shared.path());
    QVERIFY(t.probe());
    QCOMPARE(t.name(), QStringLiteral("共享目录"));

    // 空目录：清单为空
    QVERIFY(t.listRemote().isEmpty());

    // 推送
    QVERIFY(t.push(src, "报告.txt"));
    QVERIFY(QFile::exists(shared.path() + "/报告.txt"));

    QList<RemoteFileInfo> items = t.listRemote();
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.first().relPath, QStringLiteral("报告.txt"));
    QCOMPARE(items.first().size, qint64(10));
    QVERIFY(!items.first().hash.isEmpty());

    // 取回
    const QString dest = other.path() + "/报告.txt";
    QVERIFY(t.fetch("报告.txt", dest));
    QCOMPARE(readFile(dest), QByteArray("hello sync"));

    // 子目录会自动创建
    QVERIFY(t.push(src, "子目录/报告.txt"));
    QVERIFY(QFile::exists(shared.path() + "/子目录/报告.txt"));
    QCOMPARE(t.listRemote().size(), 2);

    // 清单是单个 JSON 文件，且不含 .sync 自身
    QVERIFY(QFile::exists(shared.path() + "/.sync/manifest.json"));
    QVERIFY(t.hasManifest());

    // 移除 → 从共享目录消失、清单里也没了、进回收目录
    QVERIFY(t.removeRemote("报告.txt"));
    QVERIFY(!QFile::exists(shared.path() + "/报告.txt"));
    items = t.listRemote();
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.first().relPath, QStringLiteral("子目录/报告.txt"));
    QVERIFY(QDir(shared.path() + "/.sync/trash").exists());
    QVERIFY(!QDir(shared.path() + "/.sync/trash").entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)
                 .isEmpty());
}

void TestSync::folderTransportRejectsTraversal()
{
    QTemporaryDir local, shared;
    QVERIFY(local.isValid() && shared.isValid());

    const QString src = local.path() + "/evil.txt";
    writeFile(src, "evil");

    FolderTransport t(shared.path());
    const QString outside = QFileInfo(shared.path()).absolutePath() + "/escaped.txt";

    QVERIFY(!t.push(src, "../escaped.txt"));
    QVERIFY(!QFile::exists(outside));

    QVERIFY(!t.push(src, "C:/Windows/evil.txt"));

    QVERIFY(!t.fetch("../escaped.txt", local.path() + "/out.txt"));
    QVERIFY(!QFile::exists(local.path() + "/out.txt"));

    QVERIFY(!t.removeRemote("../escaped.txt"));
}

void TestSync::folderTransportFetchMissing()
{
    QTemporaryDir local, shared;
    QVERIFY(local.isValid() && shared.isValid());

    FolderTransport t(shared.path());
    QVERIFY(!t.fetch("没有这个文件.txt", local.path() + "/x.txt"));
    QVERIFY(!QFile::exists(local.path() + "/x.txt"));
}

// ---------------------------------------------------------------- 引擎端到端
// 两台"设备"共用一个共享目录，各自有自己的本地根与基线库。
// 数据库放在本地根之外，否则 sync.db 自己也会被当成待同步文件。
//
// 注意：两台机器上"同一个文件夹"必须用**同一个别名**，否则各占一层、永远见不到
// 对方（这是产品行为，不是测试的取巧）。

namespace {

std::unique_ptr<SyncEngine> makeEngine(const QString &localRoot,
                                      const QString &alias,
                                      const QString &sharedDir,
                                      SyncDatabase *db)
{
    auto engine = std::make_unique<SyncEngine>();
    engine->setDatabase(db);
    engine->setRoots({ SyncRoot{ localRoot, alias } });
    engine->setDeviceName(QStringLiteral("测试机"));
    QList<std::shared_ptr<ITransport>> transports;
    transports.append(std::make_shared<FolderTransport>(sharedDir));
    engine->setTransports(transports);
    return engine;
}

} // namespace

void TestSync::engineTwoDevicesSequential()
{
    QTemporaryDir dirA, dirB, shared, dbDir;
    QVERIFY(dirA.isValid() && dirB.isValid() && shared.isValid() && dbDir.isValid());

    SyncDatabase dbA(QStringLiteral("eng_a"));
    SyncDatabase dbB(QStringLiteral("eng_b"));
    QVERIFY(dbA.open(dbDir.path() + "/a.db"));
    QVERIFY(dbB.open(dbDir.path() + "/b.db"));

    auto ea = makeEngine(dirA.path(), QStringLiteral("共享"), shared.path(), &dbA);
    auto eb = makeEngine(dirB.path(), QStringLiteral("共享"), shared.path(), &dbB);

    // A 新建文件 → 上传
    writeFile(dirA.path() + "/note.txt", "v1");
    SyncResult r = ea->sync();
    QCOMPARE(int(r.status), int(SyncResult::Ok));
    QCOMPARE(r.uploaded, 1);
    QCOMPARE(r.downloaded, 0);
    QVERIFY(QFile::exists(shared.path() + "/共享/note.txt"));

    // B 首次同步 → 下载（基线为空也不能判成"删除"）
    r = eb->sync();
    QCOMPARE(int(r.status), int(SyncResult::Ok));
    QCOMPARE(r.downloaded, 1);
    QCOMPARE(readFile(dirB.path() + "/note.txt"), QByteArray("v1"));

    // 双方都没改 → 一个字节都不动
    r = ea->sync();
    QCOMPARE(r.uploaded, 0);
    QCOMPARE(r.downloaded, 0);
    r = eb->sync();
    QCOMPARE(r.uploaded, 0);
    QCOMPARE(r.downloaded, 0);

    // B 改内容并同步 → 上传
    writeFile(dirB.path() + "/note.txt", "v2 from B");
    r = eb->sync();
    QCOMPARE(r.uploaded, 1);
    QCOMPARE(readFile(shared.path() + "/共享/note.txt"), QByteArray("v2 from B"));

    // A 在没同步的情况下也改了同一个文件 → 必须生成冲突副本，而不是覆盖
    writeFile(dirA.path() + "/note.txt", "v3 from A");
    r = ea->sync();
    QCOMPARE(int(r.status), int(SyncResult::Ok));
    QCOMPARE(r.conflicts, 1);

    const QStringList files = QDir(dirA.path()).entryList(QDir::Files);
    QCOMPARE(files.size(), 2);
    QVERIFY(files.contains(QStringLiteral("note.txt")));

    bool sawCopy = false;
    for (const QString &f : files) {
        if (f.contains(QStringLiteral("冲突副本")))
            sawCopy = true;
    }
    QVERIFY(sawCopy);

    QCOMPARE(dbA.recentConflicts(10).size(), 1);

    // A 再同步一次应当收敛：不重复制造冲突
    r = ea->sync();
    QCOMPARE(r.conflicts, 0);
}

void TestSync::engineSkipsWhenRemoteEmpty()
{
    QTemporaryDir dirA, shared, dbDir;
    QVERIFY(dirA.isValid() && shared.isValid() && dbDir.isValid());

    SyncDatabase dbA(QStringLiteral("eng_valve"));
    QVERIFY(dbA.open(dbDir.path() + "/a.db"));

    auto ea = makeEngine(dirA.path(), QStringLiteral("共享"), shared.path(), &dbA);

    // 先建立基线：A 上传一个文件
    writeFile(dirA.path() + "/keep.txt", "important");
    QCOMPARE(ea->sync().uploaded, 1);

    // 共享目录的清单被删掉（模拟网盘抽风/目录刚建出来）
    QVERIFY(QFile::remove(shared.path() + "/.sync/manifest.json"));

    const SyncResult r = ea->sync();
    QCOMPARE(int(r.status), int(SyncResult::RemoteUnreachable));
    // 关键：本地文件必须还在，不能被判成"对端删除"
    QVERIFY(QFile::exists(dirA.path() + "/keep.txt"));
    QVERIFY(QFile::exists(shared.path() + "/共享/keep.txt"));
}

// 两个本地文件夹里都有「报告.txt」，各自内容不同。
// 共享目录里必须分成两层，不能互相覆盖 —— 这是"多个文件夹"功能的立身之本。
void TestSync::engineTwoRootsNoCollision()
{
    QTemporaryDir docA, paperA, docB, paperB, shared, dbDir;
    QVERIFY(docA.isValid() && paperA.isValid() && docB.isValid()
            && paperB.isValid() && shared.isValid() && dbDir.isValid());

    SyncDatabase dbA(QStringLiteral("multi_a"));
    SyncDatabase dbB(QStringLiteral("multi_b"));
    QVERIFY(dbA.open(dbDir.path() + "/a.db"));
    QVERIFY(dbB.open(dbDir.path() + "/b.db"));

    auto makeTwo = [&](const QString &doc, const QString &paper, SyncDatabase *db) {
        auto e = std::make_unique<SyncEngine>();
        e->setDatabase(db);
        e->setRoots({ SyncRoot{ doc,   QStringLiteral("文档") },
                      SyncRoot{ paper, QStringLiteral("论文") } });
        e->setDeviceName(QStringLiteral("测试机"));
        QList<std::shared_ptr<ITransport>> t;
        t.append(std::make_shared<FolderTransport>(shared.path()));
        e->setTransports(t);
        return e;
    };

    auto ea = makeTwo(docA.path(), paperA.path(), &dbA);
    auto eb = makeTwo(docB.path(), paperB.path(), &dbB);

    writeFile(docA.path() + "/报告.txt", "文档里的报告");
    writeFile(paperA.path() + "/报告.txt", "论文里的报告");

    SyncResult r = ea->sync();
    QCOMPARE(int(r.status), int(SyncResult::Ok));
    QCOMPARE(r.uploaded, 2);

    QCOMPARE(readFile(shared.path() + "/文档/报告.txt"), QByteArray("文档里的报告"));
    QCOMPARE(readFile(shared.path() + "/论文/报告.txt"), QByteArray("论文里的报告"));

    // 另一台机器用同样的别名 → 两个文件夹各拿到自己那份
    r = eb->sync();
    QCOMPARE(int(r.status), int(SyncResult::Ok));
    QCOMPARE(r.downloaded, 2);
    QCOMPARE(readFile(docB.path() + "/报告.txt"), QByteArray("文档里的报告"));
    QCOMPARE(readFile(paperB.path() + "/报告.txt"), QByteArray("论文里的报告"));

    // 收敛：再同步一次什么都不动
    r = eb->sync();
    QCOMPARE(r.uploaded, 0);
    QCOMPARE(r.downloaded, 0);
}

// ---------------------------------------------------------------- 同步锁

void TestSync::folderTransportLockHeld()
{
    QTemporaryDir shared;
    QVERIFY(shared.isValid());

    FolderTransport a(shared.path());
    FolderTransport b(shared.path());

    QVERIFY(a.tryLock(0));
    QVERIFY(!b.tryLock(0));        // 独占：另一实例拿不到
    a.unlock();
    QVERIFY(b.tryLock(0));         // 释放后能拿到
    b.unlock();
}

void TestSync::folderTransportLockTakeover()
{
    QTemporaryDir shared;
    QVERIFY(shared.isValid());
    QDir().mkpath(shared.path() + "/.sync");

    const QString lock = shared.path() + "/.sync/lock.json";
    {
        QFile f(lock);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"device\":\"crashed\"}");
        // 假装上一轮同步崩溃，锁已经放了 40 秒
        QVERIFY(f.setFileTime(QDateTime::currentDateTime().addSecs(-40),
                              QFileDevice::FileModificationTime));
    }

    FolderTransport t(shared.path());
    QVERIFY(t.tryLock(0));         // 陈旧锁被接管，不是返回 false
    QVERIFY(QFile::exists(lock));
    t.unlock();
    QVERIFY(!QFile::exists(lock));
}

void TestSync::folderTransportRemoteAliases()
{
    QTemporaryDir local, shared;
    QVERIFY(local.isValid() && shared.isValid());

    const QString src = local.path() + "/a.txt";
    writeFile(src, "x");

    FolderTransport t(shared.path());
    QVERIFY(t.probe());
    QVERIFY(t.remoteAliases().isEmpty());   // 没有清单时推断不出别名

    QVERIFY(t.push(src, "share/a.txt"));
    QVERIFY(t.push(src, "照片/b.jpg"));
    QVERIFY(t.push(src, "share/c.txt"));

    // 取 relPath 第一段，去重并按字典序；同一别名出现多次只算一个
    const QStringList aliases = t.remoteAliases();
    QCOMPARE(aliases.size(), 2);
    QCOMPARE(aliases.at(0), QStringLiteral("share"));
    QCOMPARE(aliases.at(1), QStringLiteral("照片"));
}

// ---------------------------------------------------------------- 投递区

void TestSync::deliveryNameRoundTrip()
{
    const QDateTime t1 = QDateTime::fromSecsSinceEpoch(123);
    const QString name = Sync::makeDeliveryName(QStringLiteral("我的电脑"), QStringLiteral("报告 2026.txt"), t1);
    QVERIFY(name.endsWith(QStringLiteral("_报告 2026.txt")));
    QCOMPARE(Sync::stripDeliveryPrefix(name), QStringLiteral("报告 2026.txt"));

    // 设备名里的下划线会被换掉，"前缀_时间戳_原名" 的结构仍然成立
    const QString weird = Sync::makeDeliveryName(QStringLiteral("a_b/c"), QStringLiteral("x.txt"),
                                                 QDateTime::fromSecsSinceEpoch(1));
    QCOMPARE(Sync::stripDeliveryPrefix(weird), QStringLiteral("x.txt"));

    // 设备名为空时不能生成 "_时间戳_名字" 这种残废名字
    const QString noDev = Sync::makeDeliveryName(QString(), QStringLiteral("x.txt"),
                                                 QDateTime::fromSecsSinceEpoch(9));
    QVERIFY(!noDev.startsWith(QLatin1Char('_')));

    // 超长原名被截断到 120 字符以内，扩展名还在
    const QString longName = QString(300, QLatin1Char('a')) + QStringLiteral(".txt");
    const QString cut = Sync::makeDeliveryName(QStringLiteral("PC"), longName,
                                               QDateTime::fromSecsSinceEpoch(2));
    QVERIFY(cut.size() <= 120);
    QVERIFY(cut.endsWith(QStringLiteral(".txt")));

    // 新格式：投递区按设备分子目录，文件名只剩 <时间戳>_<原名>
    const QString newFmt = Sync::makeDeliveryFileName(QStringLiteral("季度 报告_v2.txt"),
                                                      QDateTime::fromSecsSinceEpoch(777));
    QVERIFY(newFmt.endsWith(QStringLiteral("_季度 报告_v2.txt")));
    QCOMPARE(Sync::stripDeliveryPrefix(newFmt), QStringLiteral("季度 报告_v2.txt"));
}

void TestSync::deliveryOwnCheck()
{
    const QString name = Sync::makeDeliveryName(QStringLiteral("我的电脑"), QStringLiteral("a.txt"),
                                                QDateTime::fromSecsSinceEpoch(5));
    QVERIFY(Sync::isOwnDelivery(name, QStringLiteral("我的电脑")));
    QVERIFY(!Sync::isOwnDelivery(name, QStringLiteral("别的电脑")));
    QVERIFY(!Sync::isOwnDelivery(QStringLiteral("没有前缀.txt"), QStringLiteral("我的电脑")));
    // 设备名里的非法字符在两侧都做了同样归一，仍能认出来
    QVERIFY(Sync::isOwnDelivery(Sync::makeDeliveryName(QStringLiteral("a_b/c"), QStringLiteral("x.txt"),
                                                       QDateTime::fromSecsSinceEpoch(1)),
                                QStringLiteral("a_b/c")));
}

void TestSync::deliveryPlaceIncoming()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString staging = dir.path() + "/_投递";
    const QString inbox   = dir.path() + "/收件箱";
    QDir().mkpath(staging);

    const QDateTime t = QDateTime::fromSecsSinceEpoch(100);
    const QString sent = staging + "/" + Sync::makeDeliveryName(QStringLiteral("A"), QStringLiteral("说明.txt"), t);
    writeFile(sent, QByteArray("内容一"));
    QVERIFY(QFile::exists(sent));

    const QString landed = Sync::placeIncoming(sent, inbox);
    QVERIFY(!landed.isEmpty());
    QCOMPARE(readFile(landed), QByteArray("内容一"));
    QCOMPARE(QFileInfo(landed).fileName(), QStringLiteral("说明.txt"));

    // 同名再进来 → 生成 "(2)" 副本，不覆盖
    const QString again = staging + "/" + Sync::makeDeliveryName(QStringLiteral("A"), QStringLiteral("说明.txt"),
                                                                 QDateTime::fromSecsSinceEpoch(200));
    writeFile(again, QByteArray("内容二"));
    QVERIFY(QFile::exists(again));

    const QString landed2 = Sync::placeIncoming(again, inbox);
    QVERIFY(!landed2.isEmpty());
    QVERIFY(landed2 != landed);
    QCOMPARE(readFile(landed2), QByteArray("内容二"));
    QCOMPARE(readFile(landed), QByteArray("内容一"));

    // 新格式：文件放在 <设备>/ 子目录里，落进收件箱时照样还原原名
    const QString senderDir = staging + "/" + Sync::sanitizeDeviceName(QStringLiteral("电脑A"));
    QDir().mkpath(senderDir);
    const QString sent3 = senderDir + "/"
                          + Sync::makeDeliveryFileName(QStringLiteral("报告_终稿.txt"),
                                                       QDateTime::fromSecsSinceEpoch(300));
    writeFile(sent3, QByteArray("内容三"));
    const QString landed3 = Sync::placeIncoming(sent3, inbox);
    QVERIFY(!landed3.isEmpty());
    QCOMPARE(QFileInfo(landed3).fileName(), QStringLiteral("报告_终稿.txt"));
    QCOMPARE(readFile(landed3), QByteArray("内容三"));
}

void TestSync::databaseReceived()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SyncDatabase db(QStringLiteral("test_received"));
    QVERIFY(db.open(dir.path() + "/sync.db"));

    QVERIFY(!db.isReceived(QStringLiteral("A_1_x.txt")));
    QVERIFY(db.markReceived(QStringLiteral("A_1_x.txt"), QStringLiteral("h1")));
    QVERIFY(db.isReceived(QStringLiteral("A_1_x.txt")));
    QVERIFY(!db.isReceived(QStringLiteral("B_1_x.txt")));
    QVERIFY(db.receivedPaths().contains(QStringLiteral("A_1_x.txt")));

    // 同一条记录重复领取不炸（ON CONFLICT DO UPDATE）
    QVERIFY(db.markReceived(QStringLiteral("A_1_x.txt"), QStringLiteral("h2")));
}

void TestSync::configWatchInboxRoundTrip()
{
    QStandardPaths::setTestModeEnabled(true);

    {
        ConfigManager cfg;
        cfg.load();
        cfg.setSyncWatchFiles(false);
        cfg.setSyncInboxDir(QStringLiteral("D:/收件箱"));
    }
    {
        ConfigManager cfg2;
        cfg2.load();
        QCOMPARE(cfg2.syncWatchFiles(), false);
        QCOMPARE(cfg2.syncInboxDir(), QStringLiteral("D:/收件箱"));
    }

    QStandardPaths::setTestModeEnabled(false);
}

// 同步根列表要能存进 config.json 再读回来。
// 用 QStandardPaths 测试模式，不碰用户真实的 %LOCALAPPDATA% 配置。
void TestSync::configRootsRoundTrip()
{
    QStandardPaths::setTestModeEnabled(true);

    {
        ConfigManager cfg;
        cfg.load();

        QVariantMap a;
        a.insert(QStringLiteral("localPath"), QStringLiteral("D:/x/文档"));
        a.insert(QStringLiteral("alias"),     QStringLiteral("文档"));
        QVariantMap b;
        b.insert(QStringLiteral("localPath"), QStringLiteral("D:/x/论文"));
        b.insert(QStringLiteral("alias"),     QStringLiteral("论文"));

        QVariantList roots;
        roots << a << b;
        cfg.setSyncRoots(roots);
        cfg.setSyncSharedDir(QStringLiteral("D:/hub"));
    }

    {
        ConfigManager cfg2;
        cfg2.load();
        const QVariantList got = cfg2.syncRoots();
        QCOMPARE(got.size(), 2);
        QCOMPARE(got.at(0).toMap().value(QStringLiteral("alias")).toString(),
                 QStringLiteral("文档"));
        QCOMPARE(got.at(1).toMap().value(QStringLiteral("localPath")).toString(),
                 QStringLiteral("D:/x/论文"));
        QCOMPARE(cfg2.syncSharedDir(), QStringLiteral("D:/hub"));
    }

    QStandardPaths::setTestModeEnabled(false);
}

QTEST_MAIN(TestSync)
#include "test_sync.moc"
