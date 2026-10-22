/// A3（采集层）验收工具。四条验收各自一个模式：
///
/// ```
///   assistant_a3 --migrate                    # ③ v1 库 → v2：加列成功、行数不变
///   assistant_a3 --prune                      # ② 工程目录剪枝：条目数下降 ≥95%
///   assistant_a3 --bench[=N]                  # ① 全量扫描吞吐（真实根 + 合成 N 条）
///   assistant_a3 --sync=<db> --tree=<dir> [--files=N]   # ④ 一次扫描+差量写库
///   assistant_a3 --cleanup=<dir>              # 收尾：删掉合成树
/// ```
///
/// ④ 的"杀进程再启动"是**两次独立进程**：第一次 CLI 建库+写差集，第二次
/// 同一个 CLI 再来一遍，第二次必须打印 `added=0 changed=0 removed=0`。
///
/// 退出码：0 = 全过；1 = 有失败项。

#include "core/FileScanner.h"
#include "database/FileIndexDatabase.h"
#include "database/DatabaseMigrator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>

#include <cstdio>

namespace {

int g_checks = 0;
int g_fails  = 0;

void report(bool ok, const QString &what)
{
    ++g_checks;
    if (ok)
        return;
    ++g_fails;
    std::printf("  FAIL  %s\n", what.toUtf8().constData());
}

QString tempPath(const QString &leaf)
{
    return QDir(QDir::tempPath()).filePath(leaf);
}

bool writeFile(const QString &path, const QByteArray &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const qint64 written = f.write(content);
    f.close();
    return written == content.size();
}

/// 造一棵确定性的合成树：`dirs` 个子目录、总共约 `files` 个文件。
bool createSyntheticTree(const QString &root, int files, int subDirs = 20)
{
    if (!QDir().mkpath(root))
        return false;

    const int perDir = files / subDirs;
    for (int d = 0; d < subDirs; ++d) {
        const QString sub = root + QStringLiteral("/sub%1").arg(d, 2, 10, QLatin1Char('0'));
        if (!QDir().mkpath(sub))
            return false;

        for (int i = 0; i < perDir; ++i) {
            if (!writeFile(sub + QStringLiteral("/file_%1_%2.dat").arg(d).arg(i), QByteArray(1, 'x')))
                return false;
        }
    }
    return true;
}

QVector<FileIndexDatabase::FileRow> scanToRows(const QStringList &roots,
                                               const FileScanner::Options &options,
                                               FileScanner::Stats *statsOut)
{
    QVector<FileScanner::Entry> entries;
    const FileScanner::Stats stats = FileScanner::scan(roots, options, &entries);
    if (statsOut)
        *statsOut = stats;

    QVector<FileIndexDatabase::FileRow> rows;
    rows.reserve(entries.size());
    for (const FileScanner::Entry &e : entries) {
        FileIndexDatabase::FileRow row;
        row.path  = e.path();
        row.name  = e.name;
        row.size  = e.size;
        row.mtime = e.mtime;
        rows.append(std::move(row));
    }
    return rows;
}

// ---------------------------------------------------------------------------
// ③ 迁移：v1 → v2
// ---------------------------------------------------------------------------

int runMigrate()
{
    std::printf("\n[migrate] v1 库 → v2（加治理字段 + idx_files_sizemtime）\n");

    const QString dbPath = tempPath(QStringLiteral("assistant-a3-migrate.db"));
    FileIndexDatabase::removeDatabaseFile(dbPath);

    constexpr int kSeedRows = 100;
    const QString seedConn = QStringLiteral("a3_seed");

    // ---- 1. 手工造一个"旧版本"库：只有 v1 schema + 数据 + user_version=1 ----
    {
        QSqlDatabase seed = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seedConn);
        seed.setDatabaseName(dbPath);
        if (!seed.open()) {
            report(false, QStringLiteral("造 v1 库：%1").arg(seed.lastError().text()));
            return 1;
        }

        QSqlQuery q(seed);
        const QStringList ddl = {
            QStringLiteral("CREATE TABLE dirs (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE)"),
            QStringLiteral("CREATE TABLE files ("
                           "  path TEXT PRIMARY KEY, name TEXT NOT NULL, name_lower TEXT NOT NULL,"
                           "  dir_id INTEGER NOT NULL REFERENCES dirs(id), ext TEXT,"
                           "  size INTEGER NOT NULL, mtime INTEGER NOT NULL,"
                           "  frecency REAL NOT NULL DEFAULT 0) WITHOUT ROWID"),
            QStringLiteral("CREATE INDEX idx_files_dir ON files(dir_id)"),
            QStringLiteral("CREATE INDEX idx_files_free ON files(frecency DESC)"),
            QStringLiteral("CREATE TABLE roots ("
                           "  path TEXT PRIMARY KEY, enabled INTEGER NOT NULL DEFAULT 1,"
                           "  last_scan INTEGER)"),
        };
        for (const QString &sql : ddl) {
            if (!q.exec(sql)) {
                report(false, QStringLiteral("v1 DDL 失败：%1").arg(q.lastError().text()));
                seed.close();
                QSqlDatabase::removeDatabase(seedConn);
                return 1;
            }
        }

        q.exec(QStringLiteral("INSERT INTO dirs (path) VALUES ('C:/seed')"));
        q.prepare(QStringLiteral(
            "INSERT INTO files (path, name, name_lower, dir_id, ext, size, mtime) "
            "VALUES (:p, :n, :nl, 1, 'dat', :s, :m)"));
        for (int i = 0; i < kSeedRows; ++i) {
            const QString name = QStringLiteral("file_%1.dat").arg(i);
            q.bindValue(QStringLiteral(":p"),  QStringLiteral("C:/seed/") + name);
            q.bindValue(QStringLiteral(":n"),  name);
            q.bindValue(QStringLiteral(":nl"), name.toLower());
            q.bindValue(QStringLiteral(":s"),  100 + i);
            q.bindValue(QStringLiteral(":m"),  1700000000 + i);
            if (!q.exec()) {
                report(false, QStringLiteral("写入种子行失败：%1").arg(q.lastError().text()));
                break;
            }
        }
        q.exec(QStringLiteral("PRAGMA user_version = 1"));
        seed.close();
    }
    QSqlDatabase::removeDatabase(seedConn);

    report(true, QStringLiteral("v1 种子库就绪（%1 行）").arg(kSeedRows));

    // ---- 2. 用 FileIndexDatabase 打开 → 它必须把库迁到 v2 ----
    {
        FileIndexDatabase db;
        const bool opened = db.open(dbPath);
        report(opened, QStringLiteral("FileIndexDatabase::open 成功（%1）").arg(db.lastError()));
        if (!opened)
            return 1;

        report(db.rowCount() == kSeedRows,
               QStringLiteral("迁移后行数不变（%1 vs %2）").arg(db.rowCount()).arg(kSeedRows));
        db.close();
    }

    // ---- 3. 独立连接复核：user_version / 新列 / 新索引 / 数据完好 ----
    const QString verifyConn = QStringLiteral("a3_verify");
    {
        QSqlDatabase v = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), verifyConn);
        v.setDatabaseName(dbPath);
        report(v.open(), QStringLiteral("复核连接打开"));

        QSqlQuery q(v);

        q.exec(QStringLiteral("PRAGMA user_version"));
        q.next();
        report(q.value(0).toInt() == 2,
               QStringLiteral("user_version == 2（实际 %1）").arg(q.value(0).toInt()));

        QStringList columns;
        q.exec(QStringLiteral("PRAGMA table_info(files)"));
        while (q.next())
            columns << q.value(1).toString();
        for (const QString &col : { QStringLiteral("cat_mask"), QStringLiteral("flags"),
                                    QStringLiteral("root_id"), QStringLiteral("origin_id") }) {
            report(columns.contains(col), QStringLiteral("新列存在：%1").arg(col));
        }
        report(columns.contains(QStringLiteral("frecency")),
               QStringLiteral("原有列没被重建掉（frecency）"));

        QStringList indexes;
        q.exec(QStringLiteral("PRAGMA index_list(files)"));
        while (q.next())
            indexes << q.value(1).toString();
        report(indexes.contains(QStringLiteral("idx_files_sizemtime")),
               QStringLiteral("新索引 idx_files_sizemtime"));

        q.exec(QStringLiteral("SELECT COUNT(*) FROM files"));
        q.next();
        report(q.value(0).toInt() == kSeedRows,
               QStringLiteral("行数仍是 %1（实际 %2）").arg(kSeedRows).arg(q.value(0).toInt()));

        q.exec(QStringLiteral("SELECT name, size FROM files WHERE path = 'C:/seed/file_7.dat'"));
        const bool hasRow = q.next();
        report(hasRow && q.value(0).toString() == QStringLiteral("file_7.dat")
                    && q.value(1).toInt() == 107,
               QStringLiteral("旧数据完好（file_7.dat / size=107）"));

        // 新列的默认值应当是 0，而不是 NULL（NOT NULL DEFAULT 0）
        q.exec(QStringLiteral("SELECT COUNT(*) FROM files WHERE cat_mask IS NULL OR flags IS NULL"));
        q.next();
        report(q.value(0).toInt() == 0, QStringLiteral("新列默认值不是 NULL"));

        q.clear();
        v.close();
    }
    // QSqlQuery / QSqlDatabase 都已析构，现在才能安全移除连接
    QSqlDatabase::removeDatabase(verifyConn);

    return 0;
}

// ---------------------------------------------------------------------------
// ② 工程目录剪枝
// ---------------------------------------------------------------------------

int runPrune()
{
    std::printf("\n[prune] 工程目录剪枝（node_modules / .git 整棵子树不进）\n");

    const QString root = tempPath(QStringLiteral("assistant-a3-prune"));
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    const int kSrcFiles  = 20;
    const int kDocsFiles = 10;
    const int kModules   = 5;
    const int kPerModule = 200;   // 1000 个
    const int kGitFiles  = 300;

    QDir().mkpath(root + "/proj/src");
    QDir().mkpath(root + "/proj/docs");
    for (int i = 0; i < kSrcFiles; ++i)
        writeFile(root + QStringLiteral("/proj/src/a%1.cpp").arg(i), "int main(){}");
    for (int i = 0; i < kDocsFiles; ++i)
        writeFile(root + QStringLiteral("/proj/docs/d%1.md").arg(i), "# doc");
    for (int m = 0; m < kModules; ++m) {
        const QString pkg = root + QStringLiteral("/proj/node_modules/pkg%1").arg(m);
        QDir().mkpath(pkg);
        for (int i = 0; i < kPerModule; ++i)
            writeFile(pkg + QStringLiteral("/m%1.js").arg(i), "x");
    }
    QDir().mkpath(root + "/proj/.git/objects");
    for (int i = 0; i < kGitFiles; ++i)
        writeFile(root + QStringLiteral("/proj/.git/objects/o%1").arg(i), "x");

    const int total = kSrcFiles + kDocsFiles + kModules * kPerModule + kGitFiles;

    // 基线：不带剪枝名单 → 应该全都扫到
    {
        FileScanner::Options raw;
        raw.excludedDirNames.clear();
        FileScanner::Stats st;
        const auto rows = scanToRows({ root }, raw, &st);
        std::printf("  不剪枝：%d 个文件（期望 %d），%d 个目录\n",
                    static_cast<int>(rows.size()), total, st.dirsVisited);
        report(rows.size() == total, QStringLiteral("不剪枝时全部扫到（%1 vs %2）")
                                         .arg(rows.size()).arg(total));
    }

    // 正式：带剪枝名单
    FileScanner::Options opt;
    opt.excludedDirNames = FileScanner::defaultExcludedDirNames();

    FileScanner::Stats st;
    const auto rows = scanToRows({ root }, opt, &st);

    const int expectedKept = kSrcFiles + kDocsFiles;
    const double dropRatio = total > 0 ? 1.0 - static_cast<double>(rows.size()) / total : 0.0;

    std::printf("  剪枝后：%d 个文件（期望 %d），剪掉 %d 棵子树，下降 %.1f%%\n",
                static_cast<int>(rows.size()), expectedKept, st.prunedDirs, dropRatio * 100.0);

    report(rows.size() == expectedKept,
           QStringLiteral("只剩非工程目录的文件（%1 vs %2）").arg(rows.size()).arg(expectedKept));
    report(dropRatio >= 0.95,
           QStringLiteral("条目数下降 ≥95%（实际 %1%）").arg(dropRatio * 100.0, 0, 'f', 1));
    report(st.prunedDirs >= 2,
           QStringLiteral("剪枝计数器记录了 ≥2 棵子树（实际 %1）").arg(st.prunedDirs));

    bool leaked = false;
    for (const auto &row : rows) {
        if (row.path.contains(QStringLiteral("node_modules"))
            || row.path.contains(QStringLiteral("/.git/")))
            leaked = true;
    }
    report(!leaked, QStringLiteral("结果里没有任何 node_modules / .git 条目"));

    return 0;
}

// ---------------------------------------------------------------------------
// ① 扫描吞吐
// ---------------------------------------------------------------------------

int runBench(int synthFiles)
{
    std::printf("\n[bench] 全量扫描吞吐\n");

    FileScanner::Options opt;
    opt.excludedDirNames = FileScanner::defaultExcludedDirNames();

    auto measure = [&](const QString &label, const QStringList &roots) {
        FileScanner::Stats st;
        const auto rows = scanToRows(roots, opt, &st);
        const double perSec = st.elapsedMs > 0
                                  ? static_cast<double>(st.files) * 1000.0 / static_cast<double>(st.elapsedMs)
                                  : 0.0;
        std::printf("  %-10s files=%-7d dirs=%-6d pruned=%-4d denied=%-3d ms=%-6lld %8.0f files/s\n",
                    label.toUtf8().constData(), st.files, st.dirsVisited,
                    st.prunedDirs, st.deniedDirs, static_cast<long long>(st.elapsedMs), perSec);

        // 外推（明确标注，别当成实测）
        if (perSec > 0.0) {
            const double extrapolated = 200000.0 / perSec * 1000.0;
            std::printf("             外推 20 万条 ≈ %.0f ms（按本机实测吞吐折算，不是实测值）\n",
                        extrapolated);
        }
        return rows.size();
    };

    const auto realRoots = FileScanner::defaultRoots();
    const int realFiles = measure(QStringLiteral("real"), realRoots);
    report(realFiles > 0, QStringLiteral("真实根扫到了文件（%1）").arg(realFiles));

    if (synthFiles > 0) {
        const QString tree = tempPath(QStringLiteral("assistant-a3-bench"));
        QDir(tree).removeRecursively();
        std::printf("  正在造合成树 %d 个文件…\n", synthFiles);
        if (!createSyntheticTree(tree, synthFiles)) {
            report(false, QStringLiteral("造合成树失败"));
            return 1;
        }
        const int got = measure(QStringLiteral("synth"), { tree });
        report(got > 0, QStringLiteral("合成树扫到了文件（%1）").arg(got));
        QDir(tree).removeRecursively();
    }

    return 0;
}

// ---------------------------------------------------------------------------
// ④ 一次扫描 + 差量写库（跑两次独立进程看收敛）
// ---------------------------------------------------------------------------

int runSync(const QString &dbPath, const QString &treeRoot, int synthFiles)
{
    std::printf("\n[sync] db=%s tree=%s\n",
                dbPath.toUtf8().constData(), treeRoot.toUtf8().constData());

    if (!QFileInfo::exists(treeRoot)) {
        std::printf("  合成树不存在，创建 %d 个文件…\n", synthFiles);
        if (!createSyntheticTree(treeRoot, synthFiles)) {
            report(false, QStringLiteral("造合成树失败"));
            return 1;
        }
    }

    FileScanner::Options opt;
    opt.excludedDirNames = FileScanner::defaultExcludedDirNames();

    FileScanner::Stats st;
    const auto rows = scanToRows({ treeRoot }, opt, &st);

    FileIndexDatabase db;
    if (!db.open(dbPath)) {
        report(false, QStringLiteral("打开库失败：%1").arg(db.lastError()));
        return 1;
    }

    const auto dbRows = db.loadAll();
    const auto diff   = FileIndexDatabase::diff(rows, dbRows);

    QElapsedTimer writeTimer;
    writeTimer.start();
    const bool written = diff.empty() ? true : db.applyDiff(diff);
    const qint64 writeMs = writeTimer.elapsed();

    const int rowsAfter = db.rowCount();
    db.close();

    report(written, QStringLiteral("applyDiff 成功（%1）").arg(db.lastError()));

    std::printf("  SYNC scan_files=%d scan_ms=%lld db_rows_before=%d "
                "added=%d changed=%d removed=%d unchanged=%d "
                "write_ms=%lld db_rows_after=%d\n",
                st.files, static_cast<long long>(st.elapsedMs),
                static_cast<int>(dbRows.size()),
                static_cast<int>(diff.added.size()), static_cast<int>(diff.changed.size()),
                static_cast<int>(diff.removed.size()), diff.unchanged,
                static_cast<long long>(writeMs), rowsAfter);

    return 0;
}

int runCleanup(const QString &treeRoot, const QString &dbPath)
{
    QDir(treeRoot).removeRecursively();
    if (!dbPath.isEmpty())
        FileIndexDatabase::removeDatabaseFile(dbPath);
    std::printf("[cleanup] removed %s\n", treeRoot.toUtf8().constData());
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Assistant"));
    QCoreApplication::setOrganizationName(QStringLiteral("AssistantTeam"));

    bool   doMigrate = false;
    bool   doPrune   = false;
    bool   doBench   = false;
    int    synthFiles = 0;
    QString syncDb;
    QString treeRoot;

    for (const QString &arg : app.arguments()) {
        if (arg == QStringLiteral("--migrate"))
            doMigrate = true;
        else if (arg == QStringLiteral("--prune"))
            doPrune = true;
        else if (arg == QStringLiteral("--bench"))
            doBench = true;
        else if (arg.startsWith(QStringLiteral("--bench="))) {
            doBench = true;
            synthFiles = arg.mid(8).toInt();
        } else if (arg.startsWith(QStringLiteral("--sync=")))
            syncDb = arg.mid(7);
        else if (arg.startsWith(QStringLiteral("--tree=")))
            treeRoot = arg.mid(7);
        else if (arg.startsWith(QStringLiteral("--files=")))
            synthFiles = arg.mid(8).toInt();
        else if (arg.startsWith(QStringLiteral("--cleanup="))) {
            runCleanup(arg.mid(10), syncDb);
            return 0;
        }
    }

    if (!syncDb.isEmpty() && !treeRoot.isEmpty())
        runSync(syncDb, treeRoot, synthFiles > 0 ? synthFiles : 2000);
    else {
        if (doMigrate) runMigrate();
        if (doPrune)   runPrune();
        if (doBench)   runBench(synthFiles);
    }

    if (!doMigrate && !doPrune && !doBench && (syncDb.isEmpty() || treeRoot.isEmpty())) {
        std::printf("用法：assistant_a3 --migrate | --prune | --bench[=N] | "
                    "--sync=<db> --tree=<dir> [--files=N]\n");
        return 1;
    }

    std::printf("\n==== %s：%d 项检查，%d 项失败 ====\n",
                g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
