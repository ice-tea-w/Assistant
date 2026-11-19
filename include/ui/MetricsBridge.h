#pragma once

#include <QObject>
#include <QVariantMap>

#include "core/MetricsService.h"

/// 数据看板页的 QML 门面（context property `metrics`）。
///
/// 只做转发：真正的聚合在 MetricsService，QML 通过 Q_INVOKABLE 拉取
/// QVariantMap。没有 Q_PROPERTY 大对象，避免每次刷新把整页绑定重新求值。
class MetricsBridge : public QObject
{
    Q_OBJECT

public:
    explicit MetricsBridge(QString dataDir, QObject *parent = nullptr);

    Q_INVOKABLE QVariantMap storage()   { return m_service.storage(); }
    Q_INVOKABLE QVariantMap downloads(int days) { return m_service.downloads(days); }
    Q_INVOKABLE QVariantMap activity(int days)  { return m_service.activity(days); }
    Q_INVOKABLE QVariantMap fileKinds()         { return m_service.fileKinds(); }
    Q_INVOKABLE QVariantMap sync()              { return m_service.sync(); }

    Q_INVOKABLE void refresh() { m_service.invalidate(); }

private:
    MetricsService m_service;
};
