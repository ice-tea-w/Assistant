#include "core/FuzzyMatcher.h"

#include <algorithm>

namespace FuzzyMatcher {

int editDistance(const QString &s1, const QString &s2)
{
    const int len1 = s1.length();
    const int len2 = s2.length();

    // Use two rows to keep memory O(min(N,M))
    // We'll always iterate over the shorter string as the inner loop.
    if (len1 == 0) return len2;
    if (len2 == 0) return len1;

    // prev = distance for s1[0..i-1], curr = distance for s1[0..i]
    std::vector<int> prev(len2 + 1);
    std::vector<int> curr(len2 + 1);

    for (int j = 0; j <= len2; ++j)
        prev[j] = j;

    for (int i = 0; i < len1; ++i) {
        curr[0] = i + 1;
        for (int j = 0; j < len2; ++j) {
            int cost = (s1[i] == s2[j]) ? 0 : 1;
            curr[j + 1] = std::min({ prev[j + 1] + 1,       // delete
                                     curr[j] + 1,           // insert
                                     prev[j] + cost });     // substitute
        }
        std::swap(prev, curr);
    }
    return prev[len2];
}

int score(const QString &query, const QString &text)
{
    if (query.isEmpty() || text.isEmpty())
        return 0;

    const QString q = query.toLower().trimmed();
    const QString t = text.toLower().trimmed();

    // ---- Tier 1: exact match ----
    if (q == t)
        return 1000;

    // ---- Tier 2: prefix match ----
    if (t.startsWith(q))
        return 900;

    // ---- Tier 3: word-boundary match ----
    // A "word boundary" is the position right after a space, hyphen, underscore,
    // or dot — or position 0 (start of the text).  We already covered position 0
    // in tier 2, so we only check interior boundaries here.
    for (int i = 1; i < t.length(); ++i) {
        const QChar prev = t[i - 1];
        if (prev == QLatin1Char(' ')  || prev == QLatin1Char('-') ||
            prev == QLatin1Char('_')  || prev == QLatin1Char('.')) {
            if (t.mid(i).startsWith(q))
                return 800;
        }
    }

    // ---- Tier 4: contains (position-weighted) ----
    int pos = t.indexOf(q);
    if (pos >= 0) {
        // 600 base, lose up to 200 points for late positions
        return 600 - std::min(pos * 2, 200);
    }

    // ---- Tier 5: Levenshtein (short strings only, guard against O(n*m)) ----
    if (q.length() <= 10 && t.length() <= 30) {
        int dist = editDistance(q, t);
        int maxLen = std::max(q.length(), t.length());
        // Only return a score when strings are at least 50 % similar.
        if (dist <= maxLen / 2) {
            return std::max(0, 400 - dist * 50);
        }
    }

    return 0;
}

} // namespace FuzzyMatcher
