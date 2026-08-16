#pragma once

#include <QString>
#include <QVariantMap>

/// Unified search result model — used by AppBridge and future modules
/// (Calculator, AppIndexer, FileSearcher) to feed results into QML.
struct SearchResult
{
    enum Type { Builtin, App, File, Calculator, Url };

    QString  id;         // unique identifier, e.g. "cmd://opencalculator"
    QString  title;      // primary display text
    QString  subtitle;   // secondary / description text (may be empty)
    Type     type    = Builtin;
    int      priority = 0;   // base ordering weight within the same score tier
    int      score    = 0;   // dynamic match score set by FuzzyMatcher::score()
    QString  resultText; // calculated result display, e.g. "= 42" (Calculator only)
    QString  path;       // launch target for indexed apps (empty for builtins)

    /// Human-readable type label (for QML display).
    static QString typeString(Type t)
    {
        switch (t) {
        case App:        return QStringLiteral("app");
        case File:       return QStringLiteral("file");
        case Calculator: return QStringLiteral("calculator");
        case Url:        return QStringLiteral("url");
        default:         return QStringLiteral("builtin");
        }
    }

    /// Convert to QVariantMap so QML can access fields as modelData.title etc.
    QVariantMap toMap() const
    {
        QVariantMap map;
        map[QStringLiteral("id")]       = id;
        map[QStringLiteral("title")]    = title;
        map[QStringLiteral("subtitle")] = subtitle;
        map[QStringLiteral("result")]   = resultText;
        map[QStringLiteral("type")]     = typeString(type);
        map[QStringLiteral("score")]    = score;
        map[QStringLiteral("path")]     = path;
        return map;
    }
};
