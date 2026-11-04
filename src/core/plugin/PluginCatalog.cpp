#include "core/plugin/PluginCatalog.h"

#include "core/Logger.h"
#include "core/plugin/PluginRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr int kSchemaVersion = 1;

/// The index document bundled into the executable. Keeping it in qrc means a fresh
/// install with no network still shows a populated market, which is the whole point
/// of the "offline seed" level.
const char *kSeedPath = ":/market/seed-index.json";

QDateTime parseTime(const json &value)
{
    if (!value.is_string())
        return QDateTime();
    return QDateTime::fromString(QString::fromStdString(value.get<std::string>()),
                                 Qt::ISODate);
}

QString readString(const json &obj, const char *key)
{
    if (!obj.contains(key) || !obj.at(key).is_string())
        return QString();
    return QString::fromStdString(obj.at(key).get<std::string>());
}

} // namespace

PluginCatalog::PluginCatalog(PluginRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
{
}

void PluginCatalog::setMarketUrl(const QString &url)
{
    m_marketUrl = url.trimmed();
}

QString PluginCatalog::cacheFile() const
{
    if (!m_registry)
        return QString();
    return m_registry->marketCacheFile();
}

// ---------------------------------------------------------------------------
// fetch
// ---------------------------------------------------------------------------

void PluginCatalog::fetch(bool force)
{
    // ---- seed-only mode (default) ----
    if (m_marketUrl.isEmpty()) {
        QList<MarketEntry> cached;
        QDateTime cachedAt;

        if (!force && loadCache(&cached, &cachedAt) && !cached.isEmpty()) {
            applyEntries(cached, /*offline=*/true, cachedAt);
            emit fetched(true, QStringLiteral("offline"));
            return;
        }

        QList<MarketEntry> seeded;
        QString error;
        if (loadSeed(&seeded, &error) && !seeded.isEmpty()) {
            const QDateTime now = QDateTime::currentDateTime();
            applyEntries(seeded, /*offline=*/true, now);
            saveCache();
            emit fetched(true, QStringLiteral("offline"));
            return;
        }

        Logger::get().warn("PluginCatalog: no seed available — {}",
                           error.toStdString());
        applyEntries({}, /*offline=*/true, QDateTime());
        emit fetched(false, QStringLiteral("no-data"));
        return;
    }

    // ---- network mode ----
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    QNetworkRequest request{QUrl(m_marketUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    if (!force && !m_etag.isEmpty())
        request.setRawHeader("If-None-Match", m_etag.toUtf8());

    QNetworkReply *reply = m_nam->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // 304: nothing changed, the cache is still authoritative.
        if (status == 304) {
            QList<MarketEntry> cached;
            QDateTime cachedAt;
            if (loadCache(&cached, &cachedAt)) {
                applyEntries(cached, /*offline=*/false, cachedAt);
                emit fetched(true, QString());
                return;
            }
            // 304 without a cache is a server bug; fall through to the error path.
        }

        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            // Degrade: cache first, then seed. Never leave the page empty if we can
            // help it, and always tell the UI that this data is stale.
            QList<MarketEntry> cached;
            QDateTime cachedAt;
            if (loadCache(&cached, &cachedAt) && !cached.isEmpty()) {
                applyEntries(cached, /*offline=*/true, cachedAt);
                emit fetched(false, QStringLiteral("network"));
                return;
            }

            QList<MarketEntry> seeded;
            QString seedError;
            if (loadSeed(&seeded, &seedError) && !seeded.isEmpty()) {
                applyEntries(seeded, /*offline=*/true, QDateTime::currentDateTime());
                emit fetched(false, QStringLiteral("network"));
                return;
            }

            applyEntries({}, /*offline=*/true, QDateTime());
            emit fetched(false, QStringLiteral("network"));
            return;
        }

        m_rawBody = reply->readAll();
        m_etag = QString::fromUtf8(reply->rawHeader("ETag"));

        QList<MarketEntry> parsed;
        QString error;
        if (!parseInto(m_rawBody, &parsed, &error)) {
            Logger::get().warn("PluginCatalog: bad index — {}", error.toStdString());
            applyEntries({}, /*offline=*/true, QDateTime());
            emit fetched(false, QStringLiteral("bad-index"));
            return;
        }

        const QDateTime now = QDateTime::currentDateTime();
        applyEntries(parsed, /*offline=*/false, now);
        saveCache();
        emit fetched(true, QString());
    });
}


// ---------------------------------------------------------------------------
// apply / query
// ---------------------------------------------------------------------------

void PluginCatalog::applyEntries(const QList<MarketEntry> &entries, bool offline,
                                 const QDateTime &fetchedAt)
{
    m_entries   = entries;
    m_offline   = offline;
    m_fetchedAt = fetchedAt;
}

QString PluginCatalog::fetchedAtText() const
{
    if (!m_offline)
        return QString();
    if (!m_fetchedAt.isValid())
        return QString();
    return m_fetchedAt.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"));
}

QStringList PluginCatalog::categories() const
{
    QSet<QString> seen;
    for (const MarketEntry &entry : m_entries) {
        for (const QString &category : entry.categories)
            seen.insert(category);
    }

    QStringList list(seen.begin(), seen.end());
    list.sort(Qt::CaseSensitive);
    return list;
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

bool PluginCatalog::parseInto(const QByteArray &raw, QList<MarketEntry> *out,
                              QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    json root;
    try {
        root = json::parse(raw.toStdString());
    } catch (const std::exception &e) {
        return fail(QStringLiteral("不是合法的 JSON：%1")
                        .arg(QString::fromUtf8(e.what())));
    }

    if (!root.is_object())
        return fail(QStringLiteral("根节点应为对象"));

    int schema = 0;
    if (root.contains("schemaVersion") && root.at("schemaVersion").is_number_integer())
        schema = root.at("schemaVersion").get<int>();

    if (schema != kSchemaVersion)
        return fail(QStringLiteral("不支持的 schemaVersion：%1").arg(schema));

    if (!root.contains("plugins") || !root.at("plugins").is_array())
        return fail(QStringLiteral("缺少 plugins 数组"));

    QList<MarketEntry> result;

    for (const json &item : root.at("plugins")) {
        if (!item.is_object())
            continue;

        MarketEntry entry;
        entry.id          = readString(item, "id");
        entry.name        = readString(item, "name");
        entry.summary     = readString(item, "summary");
        entry.description = readString(item, "description");
        entry.author      = readString(item, "author");
        entry.icon        = readString(item, "icon");

        // An entry without id or name cannot be rendered or installed; skipping is
        // better than showing a blank card.
        if (entry.id.isEmpty() || entry.name.isEmpty()) {
            Logger::get().warn("PluginCatalog: skipped an entry without id/name");
            continue;
        }

        if (item.contains("categories") && item.at("categories").is_array()) {
            for (const json &c : item.at("categories")) {
                if (c.is_string())
                    entry.categories.append(QString::fromStdString(c.get<std::string>()));
            }
        }
        if (item.contains("tags") && item.at("tags").is_array()) {
            for (const json &t : item.at("tags")) {
                if (t.is_string())
                    entry.tags.append(QString::fromStdString(t.get<std::string>()));
            }
        }

        if (item.contains("latest") && item.at("latest").is_object()) {
            const json &latest = item.at("latest");
            entry.latest.version       = readString(latest, "version");
            entry.latest.url           = readString(latest, "url");
            entry.latest.sha256        = readString(latest, "sha256");
            entry.latest.minAppVersion = readString(latest, "minAppVersion");
            entry.latest.changelog     = readString(latest, "changelog");
            if (latest.contains("size") && latest.at("size").is_number_integer())
                entry.latest.size = latest.at("size").get<qint64>();
        }

        if (entry.latest.version.isEmpty()) {
            Logger::get().warn("PluginCatalog: skipped {} — no latest.version",
                               entry.id.toStdString());
            continue;
        }

        if (item.contains("downloads") && item.at("downloads").is_number_integer())
            entry.downloads = item.at("downloads").get<qint64>();

        if (item.contains("updatedAt"))
            entry.updatedAt = parseTime(item.at("updatedAt"));

        result.append(entry);
    }

    *out = result;
    return true;
}

// ---------------------------------------------------------------------------
// seed
// ---------------------------------------------------------------------------

bool PluginCatalog::loadSeed(QList<MarketEntry> *out, QString *error)
{
    QFile file(QString::fromLatin1(kSeedPath));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取内置索引：%1").arg(file.fileName());
        return false;
    }

    const QByteArray raw = file.readAll();
    file.close();

    if (!parseInto(raw, out, error))
        return false;

    m_rawBody = raw;
    return true;
}

// ---------------------------------------------------------------------------
// cache
// ---------------------------------------------------------------------------

bool PluginCatalog::loadCache(QList<MarketEntry> *out, QDateTime *fetchedAt)
{
    const QString path = cacheFile();
    if (path.isEmpty())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QByteArray raw = file.readAll();
    file.close();

    json wrapper;
    try {
        wrapper = json::parse(raw.toStdString());
    } catch (const std::exception &) {
        return false;                 // corrupt cache is not an error, just a miss
    }

    if (!wrapper.is_object())
        return false;

    if (wrapper.contains("etag") && wrapper.at("etag").is_string())
        m_etag = QString::fromStdString(wrapper.at("etag").get<std::string>());

    QDateTime when;
    if (wrapper.contains("fetchedAt"))
        when = parseTime(wrapper.at("fetchedAt"));

    if (!wrapper.contains("index"))
        return false;

    const QByteArray indexBytes =
        QByteArray::fromStdString(wrapper.at("index").dump());

    QString error;
    if (!parseInto(indexBytes, out, &error))
        return false;

    if (fetchedAt)
        *fetchedAt = when;

    return true;
}

bool PluginCatalog::saveCache() const
{
    const QString path = cacheFile();
    if (path.isEmpty())
        return false;

    if (m_rawBody.isEmpty())
        return false;

    json index;
    try {
        index = json::parse(m_rawBody.toStdString());
    } catch (const std::exception &) {
        return false;
    }

    json wrapper;
    wrapper["schemaVersion"] = kSchemaVersion;
    wrapper["fetchedAt"] = m_fetchedAt.toUTC().toString(Qt::ISODate).toStdString();
    if (!m_etag.isEmpty())
        wrapper["etag"] = m_etag.toStdString();
    wrapper["index"] = index;

    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    const QByteArray bytes = QByteArray::fromStdString(wrapper.dump(2));
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }

    return file.commit();
}
