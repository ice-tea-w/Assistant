#pragma once

#include <QString>
#include <QStringView>

#include <optional>

/// Semantic-version parsing / comparison / range matching for plugins.
///
/// This is a deliberate **subset** of SemVer (see docs/architecture/插件市场功能设计.md §16.1).
/// We do not invent our own syntax and we do not implement the full spec either:
///
///   version : MAJOR.MINOR.PATCH  with optional "-pre"   e.g. 1.2.0-beta.1
///   compare : numeric on major/minor/patch; a version **with** pre sorts
///             BELOW the same version without pre; two pre strings compare
///             lexicographically (no dot-segment precedence rules)
///   range   : comparators  >= > <= < =  joined by spaces (AND) or "||" (OR)
///             ^1.2.3  ==  >=1.2.3 <2.0.0
///             ~1.2.3  ==  >=1.2.3 <1.3.0
///             1.x     ==  >=1.0.0 <2.0.0
///             1.2.x   ==  >=1.2.0 <1.3.0
///
///   NOT supported: "*", "latest", URLs as versions, prerelease-exclusion rules.
///
/// The whole namespace is stateless and does no I/O, so it can be unit-tested
/// without the application (same positioning as FuzzyMatcher / Calculator).
namespace semver
{
    struct Version
    {
        int     major = 0;
        int     minor = 0;
        int     patch = 0;
        QString pre;            ///< empty = release; non-empty = pre-release tag

        bool hasPre() const { return !pre.isEmpty(); }
    };

    /// Parse a strict version string ("1.2.3", "1.2.3-rc.1").
    /// Returns std::nullopt on any syntax error; wildcards are NOT accepted here.
    std::optional<Version> parse(QStringView text);

    /// Format back to "MAJOR.MINOR.PATCH[-pre]".
    QString toString(const Version &v);

    /// Three-way compare: negative if a < b, 0 if equal, positive if a > b.
    int compare(const Version &a, const Version &b);

    /// True when `v` falls inside `range`.
    /// An empty range means "no constraint" and always matches.
    bool satisfies(const Version &v, QStringView range);

    /// Validate range syntax without a concrete version.
    /// Used by PluginManifest to reject a malformed engines.assistant at parse time
    /// instead of silently matching nothing later.
    /// @param error optional human-readable reason on failure
    bool isValidRange(QStringView range, QString *error = nullptr);
}
