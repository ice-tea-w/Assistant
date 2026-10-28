#include "core/plugin/PluginRegistry.h"

#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr int kSchemaVersion = 1;

QString defaultRootDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base + QStringLiteral("/plugins");
}

/// Serialise one record. Keys are written in a fixed order so that two runs
/// produce byte-identical files (makes diffs and hand-inspection useful).
json toJson(const InstalledPlugin &p)
{
    json j;
    j["version"] = p.version.toStdString();
    j["installedAt"] = p.installedAt.toUTC().toString(Qt::ISODate).toStdString();
    j["enabled"] = p.enabled;

    json origin = json::object();
    if (!p.origin.kind.isEmpty())   origin["kind"]   = p.origin.kind.toStdString();
    if (!p.origin.url.isEmpty())    origin["url"]    = p.origin.url.toStdString();
    if (!p.origin.sha256.isEmpty()) origin["sha256"] = p.origin.sha256.toStdString();
    if (!p.origin.path.isEmpty())   origin["path"]   = p.origin.path.toStdString();
    j["origin"] = origin;

    if (p.problem.isNull()) {
        j["problem"] = nullptr;
    } else {
        json problem = json::object();
        problem["code"] = p.problem.code.toStdString();
        if (!p.problem.detail.isEmpty())
            problem["detail"] = p.problem.detail.toStdString();
        j["problem"] = problem;
    }

    return j;
}

/// Parse one record. Returns nullopt when the record is unusable, so that a single
/// damaged entry does not discard the whole registry.
std::optional<InstalledPlugin> fromJson(const QString &id, const json &j)
{
    if (!j.is_object())
        return std::nullopt;

    InstalledPlugin p;
    p.id = id;

    if (j.contains("version") && j.at("version").is_string())
        p.version = QString::fromStdString(j.at("version").get<std::string>());

    if (j.contains("installedAt") && j.at("installedAt").is_string()) {
        p.installedAt = QDateTime::fromString(
            QString::fromStdString(j.at("installedAt").get<std::string>()), Qt::ISODate);
    }

    if (j.contains("enabled") && j.at("enabled").is_boolean())
        p.enabled = j.at("enabled").get<bool>();

    if (j.contains("origin") && j.at("origin").is_object()) {
        const json &o = j.at("origin");
        const auto readInto = [&o](const char *key, QString *out) {
            if (o.contains(key) && o.at(key).is_string())
                *out = QString::fromStdString(o.at(key).get<std::string>());
        };
        readInto("kind",   &p.origin.kind);
        readInto("url",    &p.origin.url);
        readInto("sha256", &p.origin.sha256);
        readInto("path",   &p.origin.path);
    }

    if (j.contains("problem") && j.at("problem").is_object()) {
        const json &pr = j.at("problem");
        if (pr.contains("code") && pr.at("code").is_string())
            p.problem.code = QString::fromStdString(pr.at("code").get<std::string>());
        if (pr.contains("detail") && pr.at("detail").is_string())
            p.problem.detail = QString::fromStdString(pr.at("detail").get<std::string>());
    }

    // An entry without an id-key we can trust is useless; the id came from the map
    // key, so an empty one means the file itself is malformed.
    if (p.id.isEmpty())
        return std::nullopt;

    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------

PluginRegistry::PluginRegistry(const QString &rootDir)
    : m_root(rootDir.isEmpty() ? defaultRootDir() : rootDir)
{
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

QString PluginRegistry::stateFile() const
{
    return m_root + QStringLiteral("/plugins.json");
}

QString PluginRegistry::pluginDir(const QString &id) const
{
    return m_root + QLatin1Char('/') + id;
}

QString PluginRegistry::dataDir(const QString &id) const
{
    return pluginDir(id) + QStringLiteral("/data");
}

QString PluginRegistry::cacheDir() const
{
    return m_root + QStringLiteral("/.cache");
}

QString PluginRegistry::tmpDir() const
{
    return m_root + QStringLiteral("/.tmp");
}

QString PluginRegistry::logsDir() const
{
    return m_root + QStringLiteral("/.logs");
}

QString PluginRegistry::trashDir() const
{
    return m_root + QStringLiteral("/.trash");
}

QString PluginRegistry::marketCacheFile() const
{
    return m_root + QStringLiteral("/market-cache.json");
}

bool PluginRegistry::ensureRoot(QString *error) const
{
    QDir dir;
    // Create the root plus the four bookkeeping directories. They are dot-prefixed
    // so the "scan for hand-dropped plugins" step skips them.
    const QStringList dirs = {
        m_root,
        cacheDir(),
        tmpDir(),
        logsDir(),
        trashDir(),
    };

    for (const QString &path : dirs) {
        if (!dir.mkpath(path)) {
            if (error)
                *error = QStringLiteral("无法创建目录：%1").arg(path);
            return false;
        }
    }

    return true;
}

bool PluginRegistry::ensureDataDir(const QString &id, QString *path, QString *error) const
{
    const QString target = dataDir(id);

    QDir dir;
    if (!dir.mkpath(target)) {
        if (error)
            *error = QStringLiteral("无法创建插件数据目录：%1").arg(target);
        return false;
    }

    if (path)
        *path = target;

    return true;
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

bool PluginRegistry::load(QString *warning)
{
    m_plugins.clear();

    if (!ensureRoot(warning)) {
        // ensureRoot only fills the error out-param on real failures.
        return false;
    }

    const QString path = stateFile();
    QFile file(path);

    if (!file.exists()) {
        // First run: create an empty state file so the next start finds one.
        QString saveError;
        if (!save(&saveError)) {
            if (warning)
                *warning = saveError;
            // Still usable in memory; the caller can carry on.
        }
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        if (warning)
            *warning = QStringLiteral("无法读取 %1：%2").arg(path, file.errorString());
        return true;   // empty registry, but the app can run
    }

    const QByteArray raw = file.readAll();
    file.close();

    const auto quarantine = [this, &path, warning](const QString &reason) {
        const QString backup = path + QStringLiteral(".broken-")
            + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        const bool moved = QFile::rename(path, backup);
        if (warning) {
            *warning = QStringLiteral("插件状态文件损坏（%1），已%2：%3")
                           .arg(reason,
                                moved ? QStringLiteral("备份为 %1").arg(QFileInfo(backup).fileName())
                                      : QStringLiteral("备份失败"),
                                path);
        }
        Logger::get().warn("PluginRegistry: corrupt state — {}", reason.toStdString());
    };

    json root;
    try {
        root = json::parse(raw.toStdString());
    } catch (const std::exception &e) {
        quarantine(QStringLiteral("JSON 解析失败：%1").arg(QString::fromUtf8(e.what())));
        QString saveError;
        save(&saveError);
        return true;
    }

    if (!root.is_object()) {
        quarantine(QStringLiteral("根节点不是对象"));
        QString saveError;
        save(&saveError);
        return true;
    }

    int schemaVersion = 0;
    if (root.contains("schemaVersion") && root.at("schemaVersion").is_number_integer())
        schemaVersion = root.at("schemaVersion").get<int>();

    if (schemaVersion != kSchemaVersion) {
        quarantine(QStringLiteral("schemaVersion 为 %1，期望 %2")
                       .arg(schemaVersion)
                       .arg(kSchemaVersion));
        QString saveError;
        save(&saveError);
        return true;
    }

    int skipped = 0;
    if (root.contains("plugins") && root.at("plugins").is_object()) {
        for (const auto &[key, value] : root.at("plugins").items()) {
            const QString id = QString::fromStdString(key);
            const auto parsed = fromJson(id, value);
            if (!parsed) {
                ++skipped;
                continue;
            }
            m_plugins.insert(id, *parsed);
        }
    }

    if (skipped > 0) {
        if (warning) {
            *warning = QStringLiteral("插件状态里有 %1 条记录无法解析，已跳过").arg(skipped);
        }
        Logger::get().warn("PluginRegistry: skipped {} unparsable entries",
                           static_cast<size_t>(skipped));
    }

    return true;
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

bool PluginRegistry::save(QString *error) const
{
    // QSaveFile writes to a temp file and renames on commit, so a crash mid-write
    // leaves the previous plugins.json intact rather than half-written.
    QSaveFile file(stateFile());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入 %1：%2").arg(stateFile(), file.errorString());
        return false;
    }

    json root;
    root["schemaVersion"] = kSchemaVersion;
    json plugins = json::object();

    // Sorted ids keep the file stable across saves (QHash has no order).
    const QStringList sorted = ids();
    for (const QString &id : sorted)
        plugins[id.toStdString()] = toJson(m_plugins.value(id));

    root["plugins"] = plugins;

    const std::string dumped = root.dump(2);
    const QByteArray bytes = QByteArray::fromStdString(dumped);

    if (file.write(bytes) != bytes.size()) {
        if (error)
            *error = QStringLiteral("写入不完整：%1").arg(stateFile());
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("提交写入失败 %1：%2").arg(stateFile(), file.errorString());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------

std::optional<InstalledPlugin> PluginRegistry::get(const QString &id) const
{
    const auto it = m_plugins.constFind(id);
    if (it == m_plugins.constEnd())
        return std::nullopt;
    return *it;
}

QStringList PluginRegistry::ids() const
{
    QStringList list = m_plugins.keys();
    list.sort(Qt::CaseSensitive);
    return list;
}

// ---------------------------------------------------------------------------
// mutations
// ---------------------------------------------------------------------------

bool PluginRegistry::upsert(const InstalledPlugin &plugin)
{
    if (plugin.id.isEmpty())
        return false;
    m_plugins.insert(plugin.id, plugin);
    return true;
}

bool PluginRegistry::remove(const QString &id)
{
    // Qt 6 QHash::remove returns bool (Qt 5 returned the removed count).
    return m_plugins.remove(id);
}

bool PluginRegistry::upsertAndSave(const InstalledPlugin &plugin, QString *error)
{
    const QString id = plugin.id;
    const bool existed = m_plugins.contains(id);
    const InstalledPlugin previous = m_plugins.value(id);

    if (!upsert(plugin)) {
        if (error)
            *error = QStringLiteral("插件 id 为空，无法登记");
        return false;
    }

    QString saveError;
    if (!save(&saveError)) {
        // Roll memory back so the caller can treat false as "nothing happened".
        if (existed)
            m_plugins.insert(id, previous);
        else
            m_plugins.remove(id);

        if (error)
            *error = saveError;
        return false;
    }

    return true;
}

bool PluginRegistry::removeAndSave(const QString &id, QString *error)
{
    if (!m_plugins.contains(id)) {
        if (error)
            *error = QStringLiteral("插件未安装：%1").arg(id);
        return false;
    }

    const InstalledPlugin previous = m_plugins.value(id);
    m_plugins.remove(id);

    QString saveError;
    if (!save(&saveError)) {
        m_plugins.insert(id, previous);
        if (error)
            *error = saveError;
        return false;
    }

    return true;
}
