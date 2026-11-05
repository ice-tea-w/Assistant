#pragma once

#include <QString>
#include <QtGlobal>

namespace Sync::Net {

struct PeerAddress {
    QString host;
    quint16 port = 0;
    bool ok = false;
    QString error;
};

PeerAddress parsePeerAddress(const QString &input, quint16 defaultPort);

} // namespace Sync::Net
