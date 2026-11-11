#include "core/QueryParser.h"

#include "core/FileKind.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

namespace QueryParser {

namespace {

/// `10mb` / `512k` / `1.5g` / `1024` → 字节。失败返回 -1。
qint64 parseSize(QStringView text)
{
    if (text.isEmpty())
        return -1;

    QString number;
    QString unit;
    number.reserve(text.size());
    for (const QChar c : text) {
        if (c.isDigit() || c == QLatin1Char('.')) {
            if (!unit.isEmpty())
                return -1;      // 数字中间夹单位：不合法
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

/// `size:` 的右侧：`>10mb` / `<1k` / `>=1m` / `1mb..10mb`
bool parseSizeFilter(const QString &value, Parsed *out)
{
    if (value.isEmpty())
        return false;

    const int range = value.indexOf(QStringLiteral(".."));
    if (range >= 0) {
        const QString left  = value.left(range).trimmed();
        const QString right = value.mid(range + 2).trimmed();

        if (!left.isEmpty()) {
            const qint64 v = parseSize(QStringView(left));
            if (v < 0)
                return false;
            out->sizeMin = v;
        }
        if (!right.isEmpty()) {
            const qint64 v = parseSize(QStringView(right));
            if (v < 0)
                return false;
            out->sizeMax = v;
        }
        out->hasSize = true;
        return true;
    }

    QString body = value;
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
    } else if (body.startsWith(QLatin1Char('='))) {
        body = body.mid(1);
    }

    const qint64 v = parseSize(QStringView(body.trimmed()));
    if (v < 0)
        return false;

    switch (op) {
    case Op::Eq: out->sizeMin = v; out->sizeMax = v; break;
    case Op::Gt: out->sizeMin = v + 1; break;
    case Op::Ge: out->sizeMin = v; break;
    case Op::Lt: out->sizeMax = v > 0 ? v - 1 : 0; break;
    case Op::Le: out->sizeMax = v; break;
    }
    out->hasSize = true;
    return true;
}

qint64 startOfDay(const QDate &date)
{
    return QDateTime(date, QTime(0, 0), Qt::LocalTime).toSecsSinceEpoch();
}

bool parseDateFilter(const QString &value, Parsed *out)
{
    const QString v = value.trimmed();
    if (v.isEmpty())
        return false;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QDate today = QDate::currentDate();

    // 相对：today / yesterday / 7d / 24h / 30m
    if (v == QLatin1String("today")) {
        out->dateAfter = startOfDay(today);
        out->hasDate = true;
        return true;
    }
    if (v == QLatin1String("yesterday")) {
        out->dateAfter  = startOfDay(today.addDays(-1));
        out->dateBefore = startOfDay(today);
        out->hasDate = true;
        return true;
    }
    if (v.size() >= 2 && (v.endsWith(QLatin1Char('d')) || v.endsWith(QLatin1Char('h'))
                          || v.endsWith(QLatin1Char('m')))) {
        bool ok = false;
        const int n = v.left(v.size() - 1).toInt(&ok);
        if (!ok || n <= 0)
            return false;
        qint64 seconds = 0;
        switch (v.back().toLatin1()) {
        case 'd': seconds = static_cast<qint64>(n) * 86400; break;
        case 'h': seconds = static_cast<qint64>(n) * 3600;  break;
        case 'm': seconds = static_cast<qint64>(n) * 60;    break;
        default: return false;
        }
        out->dateAfter = now - seconds;
        out->hasDate = true;
        return true;
    }

    auto parseDay = [](const QString &text, QDate *date) {
        *date = QDate::fromString(text, QStringLiteral("yyyy-MM-dd"));
        return date->isValid();
    };

    const int range = v.indexOf(QStringLiteral(".."));
    if (range >= 0) {
        const QString left  = v.left(range).trimmed();
        const QString right = v.mid(range + 2).trimmed();
        QDate date;
        if (!left.isEmpty()) {
            if (!parseDay(left, &date))
                return false;
            out->dateAfter = startOfDay(date);
        }
        if (!right.isEmpty()) {
            if (!parseDay(right, &date))
                return false;
            out->dateBefore = startOfDay(date.addDays(1));
        }
        out->hasDate = true;
        return true;
    }

    QString body = v;
    enum class Op { Day, Gt, Ge, Lt, Le } op = Op::Day;
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

    QDate date;
    if (!parseDay(body.trimmed(), &date))
        return false;

    switch (op) {
    case Op::Day: out->dateAfter = startOfDay(date);
                  out->dateBefore = startOfDay(date.addDays(1)); break;
    case Op::Gt:  out->dateAfter = startOfDay(date.addDays(1)); break;
    case Op::Ge:  out->dateAfter = startOfDay(date); break;
    case Op::Lt:  out->dateBefore = startOfDay(date); break;
    case Op::Le:  out->dateBefore = startOfDay(date.addDays(1)); break;
    }
    out->hasDate = true;
    return true;
}

void appendExtensions(const QString &value, Parsed *out)
{
    const QStringList parts = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString part : parts) {
        part = part.trimmed().toLower();
        while (part.startsWith(QLatin1Char('.')))
            part.remove(0, 1);
        if (part.isEmpty() || part.contains(QLatin1Char(' ')))
            continue;
        if (!out->extensions.contains(part))
            out->extensions.append(part);
    }
}

} // namespace

Parsed parse(const QString &raw)
{
    Parsed out;

    // 整串只折叠一次；下面的切分都在这个已小写的副本上做。
    // 引号内的空格是短语的一部分，所以不能简单按空白 split。
    const QString lowered = raw.toLower();
    const int n = lowered.size();

    int i = 0;
    while (i < n) {
        while (i < n && lowered.at(i).isSpace())
            ++i;
        if (i >= n)
            break;

        // 引号短语：直到下一个引号（不闭合就吃到底）
        if (lowered.at(i) == QLatin1Char('"')) {
            ++i;
            const int start = i;
            while (i < n && lowered.at(i) != QLatin1Char('"'))
                ++i;
            const QString phrase = lowered.mid(start, i - start).trimmed();
            if (i < n)
                ++i;     // 跳过收尾引号
            if (!phrase.isEmpty())
                out.terms.append(phrase);
            continue;
        }

        const int start = i;
        while (i < n && !lowered.at(i).isSpace())
            ++i;

        const QString token = lowered.mid(start, i - start);

        if (token == QLatin1String("-"))          // 光一个减号，没有内容
            continue;

        if (token.startsWith(QLatin1Char('-'))) {
            const QString negated = token.mid(1);
            if (!negated.isEmpty())
                out.negations.append(negated);
            continue;
        }

        if (token.startsWith(QLatin1Char(':'))) {
            const uint16_t mask = FileKind::fromToken(QStringView(token).mid(1));
            if (mask != 0) {
                out.kindMask = static_cast<uint16_t>(out.kindMask | mask);
                out.hasKind = true;
            } else {
                out.terms.append(token);   // 未知 :token 降级为自由词
            }
            continue;
        }

        if (token.startsWith(QStringLiteral("ext:"))) {
            const int before = static_cast<int>(out.extensions.size());
            appendExtensions(token.mid(4), &out);
            if (static_cast<int>(out.extensions.size()) == before)
                out.terms.append(token);
            continue;
        }

        if (token.startsWith(QStringLiteral("in:"))) {
            const QString value = token.mid(3).trimmed();
            if (value.isEmpty())
                out.terms.append(token);
            else if (!out.inFilters.contains(value))
                out.inFilters.append(value);
            continue;
        }

        if (token.startsWith(QStringLiteral("size:"))) {
            if (!parseSizeFilter(token.mid(5), &out))
                out.terms.append(token);
            continue;
        }

        if (token.startsWith(QStringLiteral("date:"))) {
            if (!parseDateFilter(token.mid(5), &out))
                out.terms.append(token);
            continue;
        }

        out.terms.append(token);
    }

    return out;
}

} // namespace QueryParser
