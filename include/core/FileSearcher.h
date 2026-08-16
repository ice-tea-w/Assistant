#pragma once

#include <QString>
#include <QList>

/// Filesystem filename indexer for the assistant search bar.
///
/// Builds an in-memory index of filenames under the user's common folders
/// (Desktop, Documents, Downloads, Pictures, Music, Movies) using QDirIterator,
/// then answers fuzzy filename queries against it on every keystroke.
///
/// Deliberately a namespace (like AppIndexer/FuzzyMatcher/Calculator) rather
/// than a QObject — it has no QML surface; AppBridge mediates all access.
namespace FileSearcher
{
    /// A single indexed file.
    struct Entry
    {
        QString name;      // filename with extension, e.g. "report.docx"
        QString path;      // absolute path
        int     score = 0; // fuzzy match score, filled in by search()
    };

    /// Recursively index files under the user's common folders.
    /// Hidden/system directories are skipped; the index is capped at a fixed
    /// size to bound memory and per-keystroke cost. Returns files indexed.
    /// Never throws — logs progress via Logger.
    int buildIndex();

    /// Return up to @p limit files whose name matches @p query, best first.
    /// An empty/whitespace query returns an empty list — files never appear in
    /// the default (empty-query) result list.
    QList<Entry> search(const QString &query, int limit = 20);

    /// Number of files currently in the index.
    int size();
}
