#pragma once

#include <QString>

/// Fuzzy matching utilities for the assistant search bar.
///
/// Five-tier scoring (higher = better match):
///   1000  — exact match (case-insensitive)
///    900  — prefix match
///    800  — word-boundary match (after space / hyphen / underscore / dot)
///  600–400 — contains (earlier position = more points)
///  400–0   — Levenshtein edit distance (for short strings only)
///      0   — no match
namespace FuzzyMatcher
{
    /// Score how well `query` matches the user-visible `text`.
    /// Returns 0 when there is no meaningful match.
    int score(const QString &query, const QString &text);

    /// Levenshtein edit distance between two strings.
    int editDistance(const QString &s1, const QString &s2);
}
