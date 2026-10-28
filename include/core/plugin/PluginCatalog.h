#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class PluginRegistry;
class QNetworkAccessManager;

/// One installable entry from the market index (`index.json`, LLD §5.2).
struct MarketEntry
{
    QString id;
    QString name;
    QString summary;
    QString description;
    QString author;
    QString icon;              ///< emoji in the bundled seed; URL from a real index
    QStringList categories;
    QStringList tags;

    struct Release
    {
        QString version;
        QString url;
        QString sha256;
        qint64  size = 0;
        QString minAppVersion;
        QString changelog;
    };
    Release       latest;
    QList<Release> versions;

    qint64    downloads = 0;
    QDateTime updatedAt;
};

/// The "where do plugins come from" side of the subsystem.
///
/// Design contract (插件市场-模块框架.md §4.2): **it never throws and never returns
/// nothing when it has something older**. Three-level degradation:
///
///   network  ->  cache file  ->  bundled seed  ->  empty
///
/// `isOffline()` tells the UI which level it is on, so the banner can say
/// "显示的是 <时间> 的缓存" instead of pretending the list is fresh. "No market
/// data" and "network failed" are different facts and are reported separately.
///
/// v1 note: the default market URL is empty, so the catalog serves the seed. That is
/// deliberate — the demo must not depend on the venue's network. Point
/// ConfigManager::pluginMarketUrl at a real index to enable fetching.
class PluginCatalog : public QObject
{
    Q_OBJECT

public:
    explicit PluginCatalog(PluginRegistry *registry, QObject *parent = nullptr);

    /// Where to fetch from. Empty (default) means "seed only".
    void setMarketUrl(const QString &url);
    QString marketUrl() const { return m_marketUrl; }

    /// Load the catalog. Cached result is used when it is younger than ttlMs and
    /// `force` is false.
    void fetch(bool force = false);

    QList<MarketEntry> entries() const { return m_entries; }
    bool      isOffline() const { return m_offline; }
    QDateTime fetchedAt() const { return m_fetchedAt; }

    /// Human-readable "when is this from", for the offline banner. Empty when the
    /// data came from the network just now.
    QString fetchedAtText() const;

    /// Distinct category names across entries, sorted. Drives the filter chips, so
    /// QML does not hardcode them.
    QStringList categories() const;

    /// Where the catalog file is cached (exposed for tests).
    QString cacheFile() const;

signals:
    /// @param code empty on success; "offline" when serving cache/seed
    void fetched(bool ok, const QString &code);

private:
    /// Parse an index document. @return false when it is not usable at all.
    bool parseInto(const QByteArray &json, QList<MarketEntry> *out, QString *error) const;

    bool loadSeed(QList<MarketEntry> *out, QString *error);
    bool loadCache(QList<MarketEntry> *out, QDateTime *fetchedAt);
    bool saveCache() const;

    void applyEntries(const QList<MarketEntry> &entries, bool offline,
                      const QDateTime &fetchedAt);

    PluginRegistry *m_registry = nullptr;
    QNetworkAccessManager *m_nam = nullptr;   ///< created lazily, only for HTTP

    QString m_marketUrl;

    QList<MarketEntry> m_entries;
    bool      m_offline   = true;
    QDateTime m_fetchedAt;

    QByteArray m_rawBody;      ///< the index document as loaded (for the cache file)
    QString    m_etag;         ///< for If-None-Match on the next fetch
};
