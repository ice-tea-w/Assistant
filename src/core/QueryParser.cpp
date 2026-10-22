#include "core/QueryParser.h"

namespace QueryParser {

Parsed parse(const QString &raw)
{
    Parsed out;

    // 整串只折叠一次；下面的切分都在这个已小写的副本上做
    const QString lowered = raw.toLower();
    const int n = lowered.size();

    int i = 0;
    while (i < n) {
        while (i < n && lowered.at(i).isSpace())
            ++i;
        if (i >= n)
            break;

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
        } else {
            out.terms.append(token);
        }
    }

    return out;
}

} // namespace QueryParser
