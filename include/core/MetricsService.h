#pragma once

#include <QDateTime>
#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include <functional>

/// 只读指标聚合（数据洞察 / 图表页）。
///
/// 边界：
///  * 只读：所有 SQL 都是 SELECT；不写任何业务库；
///  * 不新增依赖：只查已有的 SQLite 文件 + QStorageInfo；
///  * 不做 UI：返回 QVariantMap，MetricsBridge / 插件 API 都可以复用。
///
/// 缓存 30 秒。所有方法都在 GUI 线程调用；查询都很小（有 created_at 索引），
/// 真正慢的目录大小扫描不在这里做。
class MetricsService
{
public:
    explicit MetricsService(QString dataDir);

    QVariantMap storage();
    QVariantMap downloads(int days);
    QVariantMap activity(int days);
    QVariantMap fileKinds();
    QVariantMap sync();

    /// 让下一次调用重新查库（插件刷新 / 页面手动刷新）。
    void invalidate();

private:
    QVariantMap cached(const QString &key, const std::function<QVariantMap()> &load);

    /// 懒打开一个只读连接；文件不存在时返回无效 QSqlDatabase，绝不创建空库。
    QSqlDatabase db(const QString &name, const QString &file);

    QString m_dataDir;
    QHash<QString, QVariantMap> m_cache;
    QHash<QString, qint64> m_cacheAt;
    QHash<QString, QSqlDatabase> m_connections;
};
