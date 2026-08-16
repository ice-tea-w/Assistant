#include "core/FileSearcher.h"
#include "core/FuzzyMatcher.h"
#include "core/Logger.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace FileSearcher {

namespace {

    /// Hard cap on the in-memory index — bounds both memory usage and the cost
    /// of the per-keystroke fuzzy pass over the whole index.
    constexpr int kMaxIndexedFiles = 20000;

    /// Directory names we never index. Hidden directories (.git, AppData) are
    /// already excluded by QDirIterator's default filter (QDir::Hidden is not
    /// set, so dot-files and hidden-attribute entries are skipped), but these
    /// names are not marked hidden and would otherwise be crawled.
    const QStringList kSkippedDirs = {
        QStringLiteral("node_modules"),
        QStringLiteral("__pycache__"),
        QStringLiteral(".cache"),
        QStringLiteral(".git"),
        QStringLiteral(".svn"),
    };

    QList<Entry> s_index;

    /// True if any path segment is one of the skipped directory names.
    bool inSkippedDir(const QString &path)
    {
        const QString native = QDir::toNativeSeparators(path);
        for (const QString &dir : kSkippedDirs) {
            if (native.contains(QDir::separator() + dir + QDir::separator()))
                return true;
        }
        return false;
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
    for (const QString &root : roots) {
        if (root.isEmpty() || seenRoots.contains(root))
            continue;
        seenRoots.insert(root);

        if (!QDir(root).exists())
            continue;

        // QDir::Files | QDir::NoDotAndDotDot — QDir::Hidden is absent, so hidden
        // files and hidden-attribute directories are skipped automatically.
        QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext() && s_index.size() < kMaxIndexedFiles) {
            it.next();
            if (inSkippedDir(it.filePath()))
                continue;

            Entry e;
            e.name = it.fileName();
            e.path = it.filePath();
            s_index.append(e);
        }

        if (s_index.size() >= kMaxIndexedFiles)
            break;
    }

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
