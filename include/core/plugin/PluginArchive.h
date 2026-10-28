#pragma once

#include <QString>

/// Safe extraction of a plugin package (.zip).
///
/// Two jobs, both security-relevant:
///
///   1. **Never write outside the target directory.** A malicious archive can name
///      an entry `../../evil.dll`, `C:/Windows/x`, `/abs` or use a backslash to dodge
///      a naive check. Every entry name is validated and the resolved path is
///      re-checked against the target root before anything is written.
///   2. **Find the real package root.** Packages are often zipped with a single
///      top-level folder (`weather-1.2.0/plugin.json`). The caller needs the directory
///      that actually holds `plugin.json`, not the extraction dir.
///
/// Qt 6 has no public zip reader (QZipReader is private and will break on a minor
/// upgrade), so this uses miniz — single-file, MIT, no dependencies.
class PluginArchive
{
public:
    struct Extracted
    {
        bool    ok = false;

        /// Empty on success, otherwise one of the LLD §7.3 codes:
        ///   "not-found" | "invalid-zip" | "zip-slip" | "not-a-plugin"
        ///   "permission" | "disk-full"
        QString code;
        QString detail;

        /// Directory containing plugin.json. Equals the extraction dir, or its
        /// single top-level subdirectory when the archive is wrapped.
        QString packageRoot;

        int fileCount = 0;
    };

    /// Extract `zipPath` into `targetDir`.
    ///
    /// `targetDir` must be a **fresh, caller-owned temporary directory**: on failure
    /// it is removed so no half-extracted tree survives.
    static Extracted extract(const QString &zipPath, const QString &targetDir);

    /// Validate one entry name.
    /// @return empty when acceptable, otherwise a human-readable reason
    static QString checkEntryName(const QString &entryName);

    /// Locate the directory holding plugin.json inside an extracted tree.
    /// @return empty when there is no plugin.json at the root or in the single subdir
    static QString findPackageRoot(const QString &extractDir);
};
