#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <functional>

/// 采集层：Win32 **迭代式 DFS**（架构文档 §3.2）。
///
/// 为什么不用 `QDirIterator`：它的过滤器只决定"返回哪些条目"，**不决定"是否递归"**，
/// 而 Windows 的 junction 在 Qt 里既不是文件、`isSymLink()` 也是 false（A1 实测踩过）。
/// 这里自己维护句柄栈：进入目录时一次判定，命中剪枝就整棵子树不进。
///
/// 三个要点：
///  1. `FindFirstFileExW` + `FindExInfoBasic` + `FIND_FIRST_EX_LARGE_FETCH`：一次系统调用
///     批量返回 `WIN32_FIND_DATAW`（含大小/时间），省掉 `QDirIterator` 每项一次 stat；
///     `FindExInfoBasic` 不取 8.3 短名。
///  2. **句柄栈而不是目录栈**：每层保留自己的 `HANDLE`，于是遍历顺序与"递归 DFS 前序"
///     **逐条相同**。这不是洁癖 —— 上限截断（§3.3 的硬保护）是按顺序截的，顺序变了
///     截出来的集合就变了，A2 的 golden 对比会跟着失真。
///  3. 长路径统一走 `\\?\` 前缀；存给上层的路径是**普通形式**（`/` 分隔），
///     这样 `QUrl::fromLocalFile` / `QDesktopServices` 与既有代码都照常工作。
namespace FileScanner {

struct Options
{
    /// 硬保护上限：到了就**停止索引**（不驱逐已有条目，§3.3）。
    int maxEntries = 1000000;
    /// 目录级剪枝：命中即整棵子树跳过（比较目录名，大小写不敏感）。
    QStringList excludedDirNames;
    /// 跳过隐藏/系统项（沿用既有语义：`QDir` 不带 `Hidden` 时 Windows 上两者都跳过）。
    bool skipHidden = true;
};

struct Entry
{
    QString dirPath;   ///< 所在目录（普通形式、`/` 分隔、无尾部斜杠）
    QString name;      ///< 文件名（含扩展名，原始大小写）
    qint64  size  = 0; ///< 字节
    qint64  mtime = 0; ///< Unix 秒

    /// 完整路径。扫描期刻意不拼串（逐条分配），只在真正需要时拼。
    QString path() const;
};

struct Stats
{
    int    files       = 0;
    int    dirsVisited = 0;
    int    prunedDirs  = 0;   ///< 被目录级剪枝跳过的子树数
    int    deniedDirs  = 0;   ///< 权限被拒 / 打不开而跳过的目录数
    bool   hitHardLimit = false;
    bool   cancelled    = false;
    qint64 elapsedMs    = 0;
};

/// 扫描多个根目录（去重；不存在或不是目录的跳过）。
/// @param out          结果追加进去（调用前清空由本函数负责）
/// @param shouldCancel 每取一个条目问一次；返回 true 就尽快收手
Stats scan(const QStringList &roots,
           const Options &options,
           QVector<Entry> *out,
           const std::function<bool()> &shouldCancel = {});

/// 常用用户目录：Desktop / Documents / Downloads / Pictures / Music / Movies。
/// 家目录本身刻意排除（里面有 config/AppData 噪音）。
QStringList defaultRoots();

/// 默认目录级剪枝名单（`node_modules` / `.git` / `.cache` / `__pycache__` / `.svn`）。
///
/// 这是**唯一**一份：旧 `FileSearcher` 也用它（A3 把原先散在两处的名单收成一处）。
/// 返回常引用 —— 遍历时每个目录都要问一次，别每次构造一个 QStringList。
const QStringList &defaultExcludedDirNames();

} // namespace FileScanner
