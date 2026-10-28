#pragma once

#include <QString>
#include <QUrl>

#include <optional>

/// "What is the user asking to install?" — parsed from a single string.
///
/// Accepted forms (see 插件市场功能设计.md §7):
///
///   weather                       -> Id          (version unknown; resolved from the market)
///   weather@1.2.0                 -> IdVersion   (exact version)
///   https://host/pkg-1.2.0.zip    -> Url         (direct download)
///   D:\tmp\pkg.zip                -> LocalPath   (side-load)
///   \\server\share\pkg.zip        -> LocalPath   (UNC)
///
/// Deliberately rejected: range specifiers in the version slot (`id@^1`), bare
/// version numbers, and anything that is neither a valid id nor a path nor a URL.
/// Deciding *what* is acceptable is cheap here; discovering it after a download is
/// not.
struct PluginSpec
{
    enum class Kind { Id, IdVersion, Url, LocalPath };

    Kind    kind = Kind::Id;
    QString id;        ///< Id / IdVersion
    QString version;   ///< IdVersion only; always a strict version, never a range
    QUrl    url;       ///< Url
    QString path;      ///< LocalPath (absolute or UNC)

    bool isLocal() const { return kind == Kind::LocalPath; }
    bool isRemote() const { return kind == Kind::Url; }
    bool needsMarketLookup() const { return kind == Kind::Id; }

    /// @param error receives a ready-to-show reason on failure ("invalid-spec")
    static std::optional<PluginSpec> parse(const QString &text, QString *error = nullptr);

    /// Same id rule as plugin.json: low-end reverse-DNS, usable as a directory name.
    static bool isValidId(const QString &id);
};
