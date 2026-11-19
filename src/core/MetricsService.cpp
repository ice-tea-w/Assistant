#include "core/MetricsService.h"

#include "core/FileKind.h"
#include "core/Logger.h"

#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QStringList>
#include <QVariantList>

#include <algorithm>

namespace {

constexpr qint64 kCacheMs = 30 * 1000;

QString dayString(const QDate &d)
{
    return d.toString(Qt::ISODate);
}

QString groupForMask(int mask)
{
    const uint16_t m = static_cast<uint16_t>(mask);
    if (m == 0) return QStringLiteral("other");

    // 文件通常只命中一个位；多选/未知时按第一个已知位归类。
    const QString token = FileKind::token(m);
    if (!token.isEmpty()) {
        if (token == "doc" || token == "sheet" || token == "slide"
            || token == "pdf" || token == "ebook")
            return QStringLiteral("doc");
        if (token == "img") return QStringLiteral("img");
        if (token == "vid") return QStringLiteral("vid");
        if (token == "aud") return QStringLiteral("aud");
        if (token == "zip" || token == "exe" || token == "disk")
            return QStringLiteral("zip");
        if (token == "code" || token == "font" || token == "data")
            return QStringLiteral("code");
        return QStringLiteral("other");
    }

    for (int bit = 0; bit < 16; ++bit) {
        if (m & (1u << bit))
            return groupForMask(1u << bit);
    }
    return QStringLiteral("other");
}

QString groupLabel(const QString &key)
{
    if (key == "doc") return QStringLiteral("文档");
    if (key == "img") return QStringLiteral("图片");
    if (key == "vid") return QStringLiteral("视频");
    if (key == "aud") return QStringLiteral("音频");
    if (key == "zip") return QStringLiteral("压缩/安装");
    if (key == "code") return QStringLiteral("代码/数据");
    return QStringLiteral("其它");
}

QVariantMap zeroKindMap()
{
    QVariantMap m;
    m.insert("doc", 0.0);
    m.insert("img", 0.0);
    m.insert("vid", 0.0);
    m.insert("aud", 0.0);
    m.insert("zip", 0.0);
    m.insert("code", 0.0);
    m.insert("other", 0.0);
    return m;
}

} // namespace

MetricsService::MetricsService(QString dataDir)
    : m_dataDir(std::move(dataDir))
{
}

void MetricsService::invalidate()
{
    m_cache.clear();
    m_cacheAt.clear();
}

QVariantMap MetricsService::cached(const QString &key,
                                   const std::function<QVariantMap()> &load)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_cache.contains(key) && now - m_cacheAt.value(key) < kCacheMs)
        return m_cache.value(key);

    QVariantMap data = load();
    data.insert(QStringLiteral("generatedAt"), QDateTime::currentSecsSinceEpoch());
    m_cache.insert(key, data);
    m_cacheAt.insert(key, now);
    return data;
}

QSqlDatabase MetricsService::db(const QString &name, const QString &file)
{
    if (m_connections.contains(name)) {
        QSqlDatabase existing = m_connections.value(name);
        if (existing.isOpen())
            return existing;
    }

    const QString path = m_dataDir + QLatin1Char('/') + file;
    if (!QFileInfo::exists(path))
        return QSqlDatabase();

    const QString connName = QStringLiteral("metrics_") + name;
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    database.setDatabaseName(path);
    database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));
    if (!database.open()) {
        Logger::get().warn("MetricsService: cannot open {}: {}",
                           path.toStdString(),
                           database.lastError().text().toStdString());
        QSqlDatabase::removeDatabase(connName);
        return QSqlDatabase();
    }
    m_connections.insert(name, database);
    return database;
}

QVariantMap MetricsService::storage()
{
    return cached(QStringLiteral("storage"), [this]() {
        QVariantMap out;
        out.insert(QStringLiteral("ok"), true);

        QVariantList volumes;
        qint64 total = 0;
        qint64 free = 0;
        qint64 used = 0;

        const auto mounted = QStorageInfo::mountedVolumes();
        for (const QStorageInfo &s : mounted) {
            if (!s.isValid() || !s.isReady() || s.bytesTotal() <= 0)
                continue;
            if (s.rootPath().startsWith(QStringLiteral("//")))
                continue; // 网络盘/UNC 可能阻塞，首版不列

            const qint64 volTotal = s.bytesTotal();
            const qint64 volFree = qMax<qint64>(0, s.bytesAvailable());
            const qint64 volUsed = qMax<qint64>(0, volTotal - volFree);

            QString label = s.rootPath();
            if (label.size() >= 2 && label.at(1) == QLatin1Char(':'))
                label = label.left(2);
            if (label.isEmpty())
                label = s.displayName();

            QVariantMap v;
            v.insert(QStringLiteral("label"), label);
            v.insert(QStringLiteral("total"), volTotal);
            v.insert(QStringLiteral("free"), volFree);
            v.insert(QStringLiteral("used"), volUsed);
            v.insert(QStringLiteral("percent"),
                     volTotal > 0 ? (100.0 * double(volUsed) / double(volTotal)) : 0.0);
            volumes.append(v);

            total += volTotal;
            free += volFree;
            used += volUsed;
        }

        out.insert(QStringLiteral("volumes"), volumes);
        out.insert(QStringLiteral("total"), total);
        out.insert(QStringLiteral("free"), free);
        out.insert(QStringLiteral("used"), used);
        out.insert(QStringLiteral("percent"),
                   total > 0 ? (100.0 * double(used) / double(total)) : 0.0);

        // 收件箱占用：从 downloads.db 读；没有库就是 0。
        QVariantMap inbox;
        inbox.insert(QStringLiteral("pendingCount"), 0);
        inbox.insert(QStringLiteral("pendingBytes"), qint64(0));
        QSqlDatabase d = db(QStringLiteral("downloads"), QStringLiteral("downloads.db"));
        if (d.isOpen()) {
            QSqlQuery q(d);
            if (q.exec(QStringLiteral("SELECT COUNT(*), COALESCE(SUM(size),0) "
                                      "FROM downloads WHERE in_box=1"))
                && q.next()) {
                inbox.insert(QStringLiteral("pendingCount"), q.value(0).toInt());
                inbox.insert(QStringLiteral("pendingBytes"), q.value(1).toLongLong());
            }
        }
        out.insert(QStringLiteral("inbox"), inbox);
        return out;
    });
}

QVariantMap MetricsService::downloads(int days)
{
    days = qBound(1, days, 365);
    const QString key = QStringLiteral("downloads:") + QString::number(days);

    return cached(key, [this, days]() {
        QVariantMap out;
        out.insert(QStringLiteral("days"), days);
        out.insert(QStringLiteral("buckets"), QVariantList());
        QVariantMap totals;
        totals.insert(QStringLiteral("count"), 0);
        totals.insert(QStringLiteral("bytes"), qint64(0));
        totals.insert(QStringLiteral("archived"), 0);
        totals.insert(QStringLiteral("byKind"), zeroKindMap());
        out.insert(QStringLiteral("totals"), totals);

        QSqlDatabase d = db(QStringLiteral("downloads"), QStringLiteral("downloads.db"));
        if (!d.isOpen()) {
            out.insert(QStringLiteral("ok"), false);
            out.insert(QStringLiteral("available"), false);
            return out;
        }

        const QDate today = QDate::currentDate();
        const QDate first = today.addDays(-(days - 1));
        QMap<QString, QVariantMap> map;
        for (int i = 0; i < days; ++i) {
            const QString day = dayString(first.addDays(i));
            QVariantMap b;
            b.insert(QStringLiteral("date"), day);
            b.insert(QStringLiteral("count"), 0);
            b.insert(QStringLiteral("bytes"), qint64(0));
            b.insert(QStringLiteral("byKind"), zeroKindMap());
            map.insert(day, b);
        }

        qint64 totalBytes = 0;
        int totalCount = 0;
        QVariantMap totalByKind = zeroKindMap();

        QSqlQuery q(d);
        q.prepare(QStringLiteral(
            "SELECT date(created_at) AS d, cat_mask, COUNT(*) AS n, "
            "COALESCE(SUM(size),0) AS bytes "
            "FROM downloads WHERE date(created_at) >= :since "
            "GROUP BY d, cat_mask ORDER BY d"));
        q.bindValue(QStringLiteral(":since"), dayString(first));
        if (q.exec()) {
            while (q.next()) {
                const QString day = q.value(0).toString();
                if (!map.contains(day))
                    continue;
                const QString group = groupForMask(q.value(1).toInt());
                const int count = q.value(2).toInt();
                const qint64 bytes = q.value(3).toLongLong();

                QVariantMap b = map.value(day);
                b.insert(QStringLiteral("count"), b.value("count").toInt() + count);
                b.insert(QStringLiteral("bytes"), b.value("bytes").toLongLong() + bytes);
                QVariantMap bk = b.value("byKind").toMap();
                bk.insert(group, bk.value(group).toDouble() + double(bytes));
                b.insert(QStringLiteral("byKind"), bk);
                map.insert(day, b);

                totalCount += count;
                totalBytes += bytes;
                totalByKind.insert(group, totalByKind.value(group).toDouble() + double(bytes));
            }
        } else {
            out.insert(QStringLiteral("error"), q.lastError().text());
        }

        QVariantList buckets;
        for (auto it = map.cbegin(); it != map.cend(); ++it)
            buckets.append(it.value());
        out.insert(QStringLiteral("buckets"), buckets);

        QSqlQuery archived(d);
        int archivedCount = 0;
        if (archived.exec(QStringLiteral(
                "SELECT COUNT(*) FROM downloads WHERE filed_at IS NOT NULL"))
            && archived.next()) {
            archivedCount = archived.value(0).toInt();
        }

        totals.insert(QStringLiteral("count"), totalCount);
        totals.insert(QStringLiteral("bytes"), totalBytes);
        totals.insert(QStringLiteral("archived"), archivedCount);
        totals.insert(QStringLiteral("byKind"), totalByKind);
        out.insert(QStringLiteral("totals"), totals);
        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("available"), true);
        return out;
    });
}

QVariantMap MetricsService::activity(int days)
{
    days = qBound(1, days, 365);
    const QString key = QStringLiteral("activity:") + QString::number(days);

    return cached(key, [this, days]() {
        QVariantMap out;
        out.insert(QStringLiteral("days"), days);

        const QDate today = QDate::currentDate();
        const QDate first = today.addDays(-(days - 1));
        const QString since = dayString(first);

        QMap<QString, QVariantMap> map;
        for (int i = 0; i < days; ++i) {
            const QString day = dayString(first.addDays(i));
            QVariantMap b;
            b.insert(QStringLiteral("date"), day);
            b.insert(QStringLiteral("searches"), 0);
            b.insert(QStringLiteral("commands"), 0);
            b.insert(QStringLiteral("launches"), 0);
            b.insert(QStringLiteral("todos"), 0);
            b.insert(QStringLiteral("notes"), 0);
            b.insert(QStringLiteral("clips"), 0);
            b.insert(QStringLiteral("downloads"), 0);
            b.insert(QStringLiteral("total"), 0);
            map.insert(day, b);
        }

        auto addSeries = [&map, &since](const QSqlDatabase &d,
                                        const QString &table,
                                        const QString &column,
                                        const QString &field) {
            if (!d.isOpen() || !d.tables().contains(table, Qt::CaseInsensitive))
                return;
            QSqlQuery q(d);
            q.prepare(QStringLiteral("SELECT date(") + column + QStringLiteral(") AS d, "
                      "COUNT(*) AS n FROM ") + table
                      + QStringLiteral(" WHERE date(") + column
                      + QStringLiteral(") >= :since GROUP BY d"));
            q.bindValue(QStringLiteral(":since"), since);
            if (!q.exec())
                return;
            while (q.next()) {
                const QString day = q.value(0).toString();
                if (!map.contains(day))
                    continue;
                QVariantMap b = map.value(day);
                b.insert(field, b.value(field).toInt() + q.value(1).toInt());
                map.insert(day, b);
            }
        };

        const QSqlDatabase history = db(QStringLiteral("history"), QStringLiteral("history.db"));
        addSeries(history, QStringLiteral("search_history"), QStringLiteral("created_at"),
                  QStringLiteral("searches"));
        addSeries(history, QStringLiteral("command_history"), QStringLiteral("created_at"),
                  QStringLiteral("commands"));
        addSeries(history, QStringLiteral("app_launch_history"), QStringLiteral("created_at"),
                  QStringLiteral("launches"));

        const QSqlDatabase todo = db(QStringLiteral("todo"), QStringLiteral("todo.db"));
        addSeries(todo, QStringLiteral("todos"), QStringLiteral("completed_at"),
                  QStringLiteral("todos"));

        const QSqlDatabase notes = db(QStringLiteral("notes"), QStringLiteral("notes.db"));
        addSeries(notes, QStringLiteral("notes"), QStringLiteral("updated_at"),
                  QStringLiteral("notes"));

        const QSqlDatabase clip = db(QStringLiteral("clipboard"), QStringLiteral("clipboard.db"));
        addSeries(clip, QStringLiteral("clipboard_history"), QStringLiteral("created_at"),
                  QStringLiteral("clips"));

        const QSqlDatabase downloads = db(QStringLiteral("downloads"), QStringLiteral("downloads.db"));
        addSeries(downloads, QStringLiteral("downloads"), QStringLiteral("created_at"),
                  QStringLiteral("downloads"));

        QVariantList buckets;
        QVariantMap totals;
        const QStringList fields = { "searches", "commands", "launches", "todos",
                                     "notes", "clips", "downloads" };
        int grandTotal = 0;
        for (auto it = map.begin(); it != map.end(); ++it) {
            QVariantMap b = it.value();
            int total = 0;
            for (const QString &f : fields) {
                const int v = b.value(f).toInt();
                total += v;
            }
            b.insert(QStringLiteral("total"), total);
            grandTotal += total;
            buckets.append(b);
        }

        for (const QString &f : fields)
            totals.insert(f, 0);
        for (const QVariant &v : buckets) {
            const QVariantMap b = v.toMap();
            for (const QString &f : fields)
                totals.insert(f, totals.value(f).toInt() + b.value(f).toInt());
        }
        totals.insert(QStringLiteral("total"), grandTotal);

        out.insert(QStringLiteral("buckets"), buckets);
        out.insert(QStringLiteral("totals"), totals);
        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("available"), grandTotal > 0);
        return out;
    });
}

QVariantMap MetricsService::fileKinds()
{
    return cached(QStringLiteral("kinds"), [this]() {
        QVariantMap out;
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("items"), QVariantList());
        out.insert(QStringLiteral("entries"), 0);
        out.insert(QStringLiteral("totalBytes"), qint64(0));

        QSqlDatabase d = db(QStringLiteral("file_index"), QStringLiteral("file_index.db"));
        if (!d.isOpen())
            return out;

        QMap<QString, QVariantMap> groups;
        qint64 totalBytes = 0;
        int entries = 0;

        QSqlQuery q(d);
        if (!q.exec(QStringLiteral(
                "SELECT cat_mask, COUNT(*), COALESCE(SUM(size),0) "
                "FROM files GROUP BY cat_mask"))) {
            out.insert(QStringLiteral("error"), q.lastError().text());
            return out;
        }
        while (q.next()) {
            const QString key = groupForMask(q.value(0).toInt());
            const int count = q.value(1).toInt();
            const qint64 bytes = q.value(2).toLongLong();
            QVariantMap g = groups.value(key);
            g.insert(QStringLiteral("key"), key);
            g.insert(QStringLiteral("label"), groupLabel(key));
            g.insert(QStringLiteral("count"), g.value("count").toInt() + count);
            g.insert(QStringLiteral("bytes"), g.value("bytes").toLongLong() + bytes);
            groups.insert(key, g);
            totalBytes += bytes;
            entries += count;
        }

        QVariantList items;
        for (auto it = groups.cbegin(); it != groups.cend(); ++it)
            items.append(it.value());
        std::sort(items.begin(), items.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value("bytes").toLongLong()
                 > b.toMap().value("bytes").toLongLong();
        });

        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("items"), items);
        out.insert(QStringLiteral("entries"), entries);
        out.insert(QStringLiteral("totalBytes"), totalBytes);
        return out;
    });
}

QVariantMap MetricsService::sync()
{
    return cached(QStringLiteral("sync"), [this]() {
        QVariantMap out;
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("syncedBytes"), qint64(0));
        out.insert(QStringLiteral("files"), 0);
        out.insert(QStringLiteral("conflicts"), 0);
        out.insert(QStringLiteral("unresolved"), 0);
        out.insert(QStringLiteral("outbox"), 0);

        QSqlDatabase d = db(QStringLiteral("sync"), QStringLiteral("sync.db"));
        if (!d.isOpen())
            return out;

        QSqlQuery q(d);
        if (q.exec(QStringLiteral("SELECT COUNT(*), COALESCE(SUM(size),0) FROM sync_state"))
            && q.next()) {
            out.insert(QStringLiteral("files"), q.value(0).toInt());
            out.insert(QStringLiteral("syncedBytes"), q.value(1).toLongLong());
        }
        if (q.exec(QStringLiteral("SELECT COUNT(*) FROM conflicts")) && q.next())
            out.insert(QStringLiteral("conflicts"), q.value(0).toInt());
        if (q.exec(QStringLiteral("SELECT COUNT(*) FROM conflicts WHERE resolved=0")) && q.next())
            out.insert(QStringLiteral("unresolved"), q.value(0).toInt());
        if (q.exec(QStringLiteral("SELECT COUNT(*) FROM outbox")) && q.next())
            out.insert(QStringLiteral("outbox"), q.value(0).toInt());

        out.insert(QStringLiteral("ok"), true);
        return out;
    });
}
