#include "ui/MetricsBridge.h"

#include <utility>

MetricsBridge::MetricsBridge(QString dataDir, QObject *parent)
    : QObject(parent)
    , m_service(std::move(dataDir))
{
}
