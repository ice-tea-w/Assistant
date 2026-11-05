#include "core/net/PeerAddress.h"

#include <QAbstractSocket>
#include <QHostAddress>

namespace Sync::Net {
namespace {

PeerAddress rejected(const QString &message)
{
    PeerAddress address;
    address.error = message;
    return address;
}

bool parsePort(const QString &text, quint16 *port)
{
    if (text.isEmpty() || text.size() > 5)
        return false;
    for (const QChar c : text) {
        if (c.unicode() < '0' || c.unicode() > '9')
            return false;
    }
    bool ok = false;
    const uint value = text.toUInt(&ok);
    if (!ok || value == 0 || value > 65535)
        return false;
    *port = quint16(value);
    return true;
}

bool isDomainName(const QString &host)
{
    if (host.isEmpty() || host.size() > 253)
        return false;
    if (host.startsWith('.') || host.endsWith('.') || host.startsWith('-') || host.endsWith('-'))
        return false;
    for (const QChar c : host) {
        const ushort u = c.unicode();
        const bool allowed = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                             || (u >= '0' && u <= '9') || u == '.' || u == '-' || u == '_';
        if (!allowed)
            return false;
    }
    return true;
}

} // namespace

PeerAddress parsePeerAddress(const QString &input, quint16 defaultPort)
{
    const QString text = input.trimmed();
    if (text.isEmpty())
        return rejected(QStringLiteral("地址不能为空"));
    if (text.size() > 255)
        return rejected(QStringLiteral("地址过长（上限 255 字符）"));
    for (const QChar c : text) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            return rejected(QStringLiteral("地址含控制字符"));
    }

    PeerAddress result;
    result.port = defaultPort;

    if (text.startsWith('[')) {
        const int close = text.indexOf(']');
        if (close < 0)
            return rejected(QStringLiteral("IPv6 地址缺少右方括号"));
        const QString host = text.mid(1, close - 1);
        if (host.isEmpty())
            return rejected(QStringLiteral("IPv6 地址为空"));

        QHostAddress address;
        if (!address.setAddress(host) || address.protocol() != QAbstractSocket::IPv6Protocol)
            return rejected(QStringLiteral("不是合法的 IPv6 地址：%1").arg(host));

        const QString rest = text.mid(close + 1);
        if (!rest.isEmpty()) {
            if (!rest.startsWith(':'))
                return rejected(QStringLiteral("右方括号后只能跟 :端口"));
            quint16 port = 0;
            if (!parsePort(rest.mid(1), &port))
                return rejected(QStringLiteral("端口非法：%1").arg(rest.mid(1)));
            result.port = port;
        }
        result.host = host;
        result.ok = true;
        return result;
    }

    const int colon = text.indexOf(':');
    if (colon >= 0 && text.indexOf(':', colon + 1) >= 0) {
        QHostAddress address;
        if (address.setAddress(text))
            return rejected(QStringLiteral("IPv6 地址必须加方括号，例如 [2408::1]:45871"));
        return rejected(QStringLiteral("地址含多个冒号，无法解析"));
    }

    QString host = text;
    if (colon >= 0) {
        host = text.left(colon);
        quint16 port = 0;
        if (!parsePort(text.mid(colon + 1), &port))
            return rejected(QStringLiteral("端口非法：%1").arg(text.mid(colon + 1)));
        result.port = port;
    }
    if (host.isEmpty())
        return rejected(QStringLiteral("主机为空"));

    QHostAddress address;
    const bool isV4 = address.setAddress(host)
                      && address.protocol() == QAbstractSocket::IPv4Protocol;
    if (!isV4 && !isDomainName(host))
        return rejected(QStringLiteral("主机名非法：%1").arg(host));

    result.host = host;
    result.ok = true;
    return result;
}

} // namespace Sync::Net
