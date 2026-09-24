#include "core/FileSearcher.h"
#include "core/FuzzyMatcher.h"
#include "core/Logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace FileSearcher {

namespace {

    /// Hard cap on the in-memory index — bounds both memory usage and the cost
    /// of the per-keystroke fuzzy pass over the whole index.
    constexpr int kMaxIndexedFiles = 20000;

    /// Directory names we never index. Hidden directories (.git, AppData) are
    /// already excluded because QDir::Hidden is not part of the listing filter
    /// (dot-files and hidden-attribute entries are skipped), but these names are
    /// not marked hidden and would otherwise be crawled.
    const QStringList kSkippedDirs = {
        QStringLiteral("node_modules"),
        QStringLiteral("__pycache__"),
        QStringLiteral(".cache"),
        QStringLiteral(".git"),
        QStringLiteral(".svn"),
    };

    QList<Entry> s_index;
    int s_prunedDirs = 0;

    /// True if this **directory name** is one we never descend into.
    ///
    /// A1 修 P0-2：判定只比较「段名」本身，不再为每条文件拼路径、做
    /// QDir::toNativeSeparators()（每条分配一个 QString）或 toLower()（又分配一个）。
    /// 旧的实现压在遍历最内层循环上，2 万条文件要白白分配 2 万个字符串。
    bool isSkippedDirName(const QString &name)
    {
        return kSkippedDirs.contains(name, Qt::CaseInsensitive);
    }

    /// 深度优先遍历一个目录（前序，与旧 QDirIterator 的顺序一致）。
    ///
    /// **为什么不用 QDirIterator**：QDirIterator 没有「不下降进某个子目录」的开关，
    /// 而 `QDir::NoSymLinks` 在 Windows 上**不认 junction**——Qt 把 junction 当普通
    /// 目录（既不是文件，`isSymLink()` 也返回 false）。实测在 Desktop 放一个指回
    /// 父目录的 junction，索引里会出现 63 层 `loop/loop/…/real/` 的重复条目，
    /// 把 2 万条上限白白吃掉。所以 P0-1/P0-2 都只能在「是否递归」这一层修，
    /// 遍历必须自己控制。A3 会把这里换成 FindFirstFileExW 的迭代式栈。
    void walkDir(const QString &dirPath)
    {
        const QFileInfoList entries =
            QDir(dirPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                        QDir::NoSort);   // NoSort = 保持文件系统原始顺序
        for (const QFileInfo &info : entries) {
            if (s_index.size() >= kMaxIndexedFiles)
                return;

            if (info.isDir()) {
                // P0-1：不跟随 reparse point（junction 与符号链接都不是"真目录"）
                if (info.isSymLink() || info.isJunction())
                    continue;

                // P0-2：命中即整棵子树跳过——剪枝发生在**下降之前**，
                // 子目录里的文件一个都不会被枚举，而不是逐条回看整条路径。
                if (isSkippedDirName(info.fileName())) {
                    ++s_prunedDirs;
                    continue;
                }

                walkDir(info.filePath());
            } else {
                if (info.isSymLink())
                    continue;

                Entry e;
                e.name = info.fileName();
                e.path = info.filePath();
                s_index.append(e);
            }
        }
    }

} // namespace

int buildIndex()
{
    s_index.clear();

    // Common user folders — the same set a launcher would surface. The home
    // root itself is intentionally excluded (it contains config/AppData noise).
    const QStringList roots = {
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
    };

    QElapsedTimer timer;
    timer.start();

    QSet<QString> seenRoots;   // cheap insurance against overlapping roots
    s_prunedDirs = 0;
    for (const QString &root : roots) {
        if (root.isEmpty() || seenRoots.contains(root))
            continue;
        seenRoots.insert(root);

        if (!QDir(root).exists())
            continue;

        // QDir::Hidden 故意不设 → 隐藏文件与隐藏目录自动跳过（沿用既有语义）
        walkDir(root);

        if (s_index.size() >= kMaxIndexedFiles)
            break;
    }

    if (s_prunedDirs > 0)
        Logger::get().debug("FileSearcher: pruned {} excluded dir(s) (node_modules etc.)",
                            s_prunedDirs);

    if (s_index.size() >= kMaxIndexedFiles)
        Logger::get().warn("FileSearcher: index capped at {} files",
                           kMaxIndexedFiles);

    Logger::get().info("FileSearcher: indexed {} files in {} ms", s_index.size(),
                       timer.elapsed());
    return s_index.size();
}

QList<Entry> search(const QString &query, int limit)
{
    QList<Entry> matches;
    if (query.trimmed().isEmpty() || limit <= 0)
        return matches;

    // Score every filename against the query. Short filenames keep this cheap;
    // the index cap keeps the worst case bounded.
    matches.reserve(1024);
    for (const Entry &e : s_index) {
        const int s = FuzzyMatcher::score(query, e.name);
        if (s > 0) {
            Entry m = e;
            m.score = s;
            matches.append(m);
        }
    }

    // Best score first; name breaks ties for a stable, friendly order.
    std::sort(matches.begin(), matches.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.score != b.score)
                      return a.score > b.score;
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });

    if (matches.size() > limit)
        matches.erase(matches.begin() + limit, matches.end());

    return matches;
}

int size()
{
    return s_index.size();
}

} // namespace FileSearcher
