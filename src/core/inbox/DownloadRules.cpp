#include "core/inbox/DownloadRules.h"

#include "core/FileKind.h"
#include "database/DownloadsDatabase.h"

#include <QDate>
#include <QDir>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>

namespace {

bool parseSizeFilter(const QString &value, qint64 *minOut, qint64 *maxOut)
{
    const QString v = value.trimmed().toLower();
    if (v.isEmpty())
        return false;

    const int range = v.indexOf(QStringLiteral(".."));
    if (range >= 0) {
        const QString left = v.left(range).trimmed();
        const QString right = v.mid(range + 2).trimmed();
        if (!left.isEmpty()) {
            const qint64 bytes = DownloadRules::parseSize(QStringView(left));
            if (bytes < 0)
                return false;
            *minOut = bytes;
        }
        if (!right.isEmpty()) {
            const qint64 bytes = DownloadRules::parseSize(QStringView(right));
            if (bytes < 0)
                return false;
            *maxOut = bytes;
        }
        return true;
    }

    QString body = v;
    enum class Op { Eq, Gt, Ge, Lt, Le } op = Op::Eq;
    if (body.startsWith(QStringLiteral(">="))) {
        op = Op::Ge;
        body = body.mid(2);
    } else if (body.startsWith(QLatin1Char('>'))) {
        op = Op::Gt;
        body = body.mid(1);
    } else if (body.startsWith(QStringLiteral("<="))) {
        op = Op::Le;
        body = body.mid(2);
    } else if (body.startsWith(QLatin1Char('<'))) {
        op = Op::Lt;
        body = body.mid(1);
    }

    const qint64 bytes = DownloadRules::parseSize(QStringView(body.trimmed()));
    if (bytes < 0)
        return false;

    switch (op) {
    case Op::Eq: *minOut = bytes; *maxOut = bytes; break;
    case Op::Gt: *minOut = bytes + 1; break;
    case Op::Ge: *minOut = bytes; break;
    case Op::Lt: *maxOut = bytes > 0 ? bytes - 1 : 0; break;
    case Op::Le: *maxOut = bytes; break;
    }
    return true;
}

} // namespace

void DownloadRules::load(const QVariantList &rules)
{
    m_rules.clear();

    for (const QVariant &item : rules) {
        const QVariantMap map = item.toMap();
        if (map.isEmpty())
            continue;

        Rule rule;
        rule.name = map.value(QStringLiteral("name")).toString();
        rule.priority = map.value(QStringLiteral("priority"), 100).toInt();
        rule.enabled = map.value(QStringLiteral("enabled"), true).toBool();
        rule.action = map.value(QStringLiteral("action"), QStringLiteral("suggest")).toString().toLower();
        rule.dest = map.value(QStringLiteral("dest")).toString();

        const QString kind = map.value(QStringLiteral("kind")).toString();
        for (const QString &token : kind.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            rule.kindMask = static_cast<uint16_t>(
                rule.kindMask | FileKind::fromToken(QStringView(token.trimmed())));
        }

        parseSizeFilter(map.value(QStringLiteral("size")).toString(),
                        &rule.minSize, &rule.maxSize);

        if (rule.name.isEmpty())
            rule.name = QStringLiteral("rule-%1").arg(m_rules.size() + 1);
        m_rules.append(rule);
    }

    std::sort(m_rules.begin(), m_rules.end(), [](const Rule &a, const Rule &b) {
        return a.priority < b.priority;
    });
}

DownloadRules::Decision DownloadRules::evaluate(const DownloadRow &row,
                                                const QString &boxPath) const
{
    for (const Rule &rule : m_rules) {
        if (!rule.enabled)
            continue;
        if (rule.kindMask != 0 && (row.catMask & rule.kindMask) == 0)
            continue;
        if (rule.minSize >= 0 && row.size < rule.minSize)
            continue;
        if (rule.maxSize >= 0 && row.size > rule.maxSize)
            continue;

        Decision d;
        d.action = rule.action;
        d.ruleName = rule.name;
        d.dest = rule.dest.isEmpty() ? QString() : expand(rule.dest, row, boxPath);
        return d;
    }
    return {};
}

QString DownloadRules::expand(const QString &tpl, const DownloadRow &row,
                              const QString &boxPath)
{
    const QDate today = QDate::currentDate();
    QString out = tpl;
    out.replace(QStringLiteral("{inbox}"), QDir::fromNativeSeparators(boxPath));
    out.replace(QStringLiteral("{name}"), row.name);
    out.replace(QStringLiteral("{ext}"), row.ext);
    out.replace(QStringLiteral("{kind}"), FileKind::token(row.catMask));
    out.replace(QStringLiteral("{yyyy}"), QString::number(today.year()));
    out.replace(QStringLiteral("{MM}"), QStringLiteral("%1").arg(today.month(), 2, 10, QLatin1Char('0')));
    out.replace(QStringLiteral("{dd}"), QStringLiteral("%1").arg(today.day(), 2, 10, QLatin1Char('0')));
    out.replace(QStringLiteral("{source}"), row.sourceId);
    return QDir::cleanPath(QDir::fromNativeSeparators(out));
}

qint64 DownloadRules::parseSize(QStringView text)
{
    QString number;
    QString unit;
    for (const QChar c : text) {
        if (c.isDigit() || c == QLatin1Char('.')) {
            if (!unit.isEmpty())
                return -1;
            number.append(c);
        } else if (!c.isSpace()) {
            unit.append(c.toLower());
        }
    }
    if (number.isEmpty())
        return -1;

    bool ok = false;
    const double value = number.toDouble(&ok);
    if (!ok || value < 0)
        return -1;

    qint64 factor = 1;
    if (unit.isEmpty() || unit == QLatin1String("b"))
        factor = 1;
    else if (unit == QLatin1String("k") || unit == QLatin1String("kb"))
        factor = 1024LL;
    else if (unit == QLatin1String("m") || unit == QLatin1String("mb"))
        factor = 1024LL * 1024;
    else if (unit == QLatin1String("g") || unit == QLatin1String("gb"))
        factor = 1024LL * 1024 * 1024;
    else if (unit == QLatin1String("t") || unit == QLatin1String("tb"))
        factor = 1024LL * 1024 * 1024 * 1024;
    else
        return -1;

    return static_cast<qint64>(value * static_cast<double>(factor));
}
