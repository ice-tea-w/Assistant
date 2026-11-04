#include "core/plugin/SemVer.h"

#include <QStringList>

namespace semver {

namespace {

/// Outcome of evaluating a single comparator token.
/// The distinction between Invalid and NoMatch matters: "the range is malformed"
/// and "the version does not satisfy a well-formed range" must not be conflated,
/// otherwise a typo in engines.assistant silently matches nothing forever.
enum class TokenResult { Invalid, NoMatch, Match };

/// A numeric component must be all ASCII digits and short enough to fit an int.
bool parseNumber(QStringView s, int *out)
{
    if (s.isEmpty() || s.size() > 9)
        return false;

    int value = 0;
    for (const QChar c : s) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return false;
        value = value * 10 + (c.unicode() - '0');
    }

    *out = value;
    return true;
}

/// Split "MAJOR.MINOR.PATCH" into three numbers. Exactly three parts are required;
/// wildcards are rejected here on purpose.
bool parseCore(const QString &core, int *major, int *minor, int *patch)
{
    const QStringList parts = core.split(QLatin1Char('.'));
    if (parts.size() != 3)
        return false;

    return parseNumber(QStringView(parts.at(0)), major)
        && parseNumber(QStringView(parts.at(1)), minor)
        && parseNumber(QStringView(parts.at(2)), patch);
}

bool isWildcardComponent(const QString &component)
{
    return component == QLatin1String("x") || component == QLatin1String("X");
}

/// Parse `text` as a strict version, writing to `out` on success.
bool parseInto(const QString &text, Version *out)
{
    const auto parsed = parse(QStringView(text));
    if (!parsed)
        return false;
    *out = *parsed;
    return true;
}

/// `1.x`   -> major must equal 1
/// `1.2.x` -> major.minor must equal 1.2
bool matchWildcard(const Version &v, const QString &token)
{
    const QStringList parts = token.split(QLatin1Char('.'));

    if (parts.size() == 2 && isWildcardComponent(parts.at(1))) {
        int major = 0;
        if (!parseNumber(QStringView(parts.at(0)), &major))
            return false;
        return v.major == major;
    }

    if (parts.size() == 3 && isWildcardComponent(parts.at(2))) {
        int major = 0;
        int minor = 0;
        if (!parseNumber(QStringView(parts.at(0)), &major)
            || !parseNumber(QStringView(parts.at(1)), &minor))
            return false;
        return v.major == major && v.minor == minor;
    }

    return false;
}

/// True for the two supported wildcard shapes only, so a pre-release tag that
/// happens to contain the letter x is not mistaken for one.
bool isWildcardToken(const QString &token)
{
    const QStringList parts = token.split(QLatin1Char('.'));
    if (parts.size() == 2) {
        int major = 0;
        return isWildcardComponent(parts.at(1))
            && parseNumber(QStringView(parts.at(0)), &major);
    }
    if (parts.size() == 3) {
        int major = 0;
        int minor = 0;
        return isWildcardComponent(parts.at(2))
            && parseNumber(QStringView(parts.at(0)), &major)
            && parseNumber(QStringView(parts.at(1)), &minor);
    }
    return false;
}

/// `^X.Y.Z` -> >=X.Y.Z and <(X+1).0.0
TokenResult matchCaret(const Version &v, const QString &body)
{
    Version base;
    if (!parseInto(body, &base))
        return TokenResult::Invalid;

    Version upper = base;
    upper.major += 1;
    upper.minor = 0;
    upper.patch = 0;
    upper.pre.clear();

    const bool ok = compare(v, base) >= 0 && compare(v, upper) < 0;
    return ok ? TokenResult::Match : TokenResult::NoMatch;
}

/// `~X.Y.Z` -> >=X.Y.Z and <X.(Y+1).0
TokenResult matchTilde(const Version &v, const QString &body)
{
    Version base;
    if (!parseInto(body, &base))
        return TokenResult::Invalid;

    Version upper = base;
    upper.minor += 1;
    upper.patch = 0;
    upper.pre.clear();

    const bool ok = compare(v, base) >= 0 && compare(v, upper) < 0;
    return ok ? TokenResult::Match : TokenResult::NoMatch;
}

/// Evaluate one space-separated comparator token.
TokenResult matchToken(const Version &v, const QString &token)
{
    if (token.isEmpty())
        return TokenResult::Match;

    const auto sameAs = [](int cmp) {
        return cmp == 0 ? TokenResult::Match : TokenResult::NoMatch;
    };

    // Two-character operators must be tested before one-character ones.
    if (token.startsWith(QLatin1String(">="))) {
        Version rhs;
        if (!parseInto(token.mid(2).trimmed(), &rhs)) return TokenResult::Invalid;
        return compare(v, rhs) >= 0 ? TokenResult::Match : TokenResult::NoMatch;
    }
    if (token.startsWith(QLatin1String("<="))) {
        Version rhs;
        if (!parseInto(token.mid(2).trimmed(), &rhs)) return TokenResult::Invalid;
        return compare(v, rhs) <= 0 ? TokenResult::Match : TokenResult::NoMatch;
    }
    if (token.startsWith(QLatin1Char('>'))) {
        Version rhs;
        if (!parseInto(token.mid(1).trimmed(), &rhs)) return TokenResult::Invalid;
        return compare(v, rhs) > 0 ? TokenResult::Match : TokenResult::NoMatch;
    }
    if (token.startsWith(QLatin1Char('<'))) {
        Version rhs;
        if (!parseInto(token.mid(1).trimmed(), &rhs)) return TokenResult::Invalid;
        return compare(v, rhs) < 0 ? TokenResult::Match : TokenResult::NoMatch;
    }
    if (token.startsWith(QLatin1Char('='))) {
        Version rhs;
        if (!parseInto(token.mid(1).trimmed(), &rhs)) return TokenResult::Invalid;
        return sameAs(compare(v, rhs));
    }
    if (token.startsWith(QLatin1Char('^')))
        return matchCaret(v, token.mid(1).trimmed());
    if (token.startsWith(QLatin1Char('~')))
        return matchTilde(v, token.mid(1).trimmed());

    if (isWildcardToken(token))
        return matchWildcard(v, token) ? TokenResult::Match : TokenResult::NoMatch;

    // A bare version means exact equality.
    Version exact;
    if (!parseInto(token, &exact))
        return TokenResult::Invalid;
    return sameAs(compare(v, exact));
}

/// Split a range into OR-separated groups.
QStringList splitGroups(const QString &range)
{
    return range.split(QLatin1String("||"));
}

/// Split one group into AND-separated comparator tokens.
QStringList splitTokens(const QString &group)
{
    // simplified() collapses tabs / newlines / repeated spaces into single spaces.
    return group.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

} // namespace

// ---------------------------------------------------------------------------
// parse
// ---------------------------------------------------------------------------

std::optional<Version> parse(QStringView text)
{
    const QString s = text.trimmed().toString();
    if (s.isEmpty())
        return std::nullopt;

    QString core = s;
    QString pre;

    const int dash = s.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        core = s.left(dash);
        pre  = s.mid(dash + 1);

        // A trailing "-" must be followed by something, and the tag is limited to
        // dot-separated alphanumerics plus inner dashes.
        if (pre.isEmpty())
            return std::nullopt;

        for (const QChar c : pre) {
            if (!(c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('-')))
                return std::nullopt;
        }
    }

    Version v;
    if (!parseCore(core, &v.major, &v.minor, &v.patch))
        return std::nullopt;

    v.pre = pre;
    return v;
}

// ---------------------------------------------------------------------------
// toString
// ---------------------------------------------------------------------------

QString toString(const Version &v)
{
    QString out = QStringLiteral("%1.%2.%3").arg(v.major).arg(v.minor).arg(v.patch);
    if (!v.pre.isEmpty())
        out += QLatin1Char('-') + v.pre;
    return out;
}

// ---------------------------------------------------------------------------
// compare
// ---------------------------------------------------------------------------

int compare(const Version &a, const Version &b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;

    // A pre-release sorts BELOW the corresponding release.
    if (a.pre.isEmpty() && b.pre.isEmpty()) return 0;
    if (a.pre.isEmpty()) return 1;
    if (b.pre.isEmpty()) return -1;

    const int c = QString::compare(a.pre, b.pre, Qt::CaseSensitive);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

// ---------------------------------------------------------------------------
// satisfies
// ---------------------------------------------------------------------------

bool satisfies(const Version &v, QStringView range)
{
    const QString r = range.trimmed().toString();
    if (r.isEmpty())
        return true;                       // no constraint

    const QStringList groups = splitGroups(r);
    for (const QString &group : groups) {
        const QStringList tokens = splitTokens(group);
        if (tokens.isEmpty())
            continue;

        bool groupMatches = true;
        for (const QString &token : tokens) {
            const TokenResult res = matchToken(v, token);
            if (res != TokenResult::Match) {
                groupMatches = false;
                break;
            }
        }

        if (groupMatches)
            return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// isValidRange
// ---------------------------------------------------------------------------

bool isValidRange(QStringView range, QString *error)
{
    const QString r = range.trimmed().toString();

    if (r.isEmpty()) {
        if (error) *error = QStringLiteral("版本范围为空");
        return false;
    }

    // "*" is explicitly outside our subset.
    if (r.contains(QLatin1Char('*'))) {
        if (error) *error = QStringLiteral("不支持通配符 *，请改用 1.x 或 1.2.x");
        return false;
    }

    const QStringList groups = splitGroups(r);
    for (const QString &group : groups) {
        const QStringList tokens = splitTokens(group);
        if (tokens.isEmpty()) {
            if (error) *error = QStringLiteral("版本范围里有空的比较项");
            return false;
        }

        for (const QString &token : tokens) {
            if (matchToken(Version{}, token) == TokenResult::Invalid) {
                if (error) *error = QStringLiteral("无法识别的版本比较项：%1").arg(token);
                return false;
            }
        }
    }

    return true;
}

} // namespace semver
