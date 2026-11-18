#include "core/plugin/PluginHost.h"

#include "core/Logger.h"
#include "core/plugin/PluginDisposable.h"
#include "core/plugin/PluginExtensionRegistry.h"
#include "core/plugin/PluginServices.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QSaveFile>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

QString settingsFileOf(const QString &dataDir)
{
    return dataDir + QStringLiteral("/settings.json");
}

/// QVariant -> nlohmann. Supports the types QML can actually hand us.
json variantToJson(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::Bool:          return value.toBool();
    case QMetaType::Int:
    case QMetaType::LongLong:      return value.toLongLong();
    case QMetaType::UInt:
    case QMetaType::ULongLong:     return static_cast<qint64>(value.toULongLong());
    case QMetaType::Double:        return value.toDouble();
    case QMetaType::QString:       return value.toString().toStdString();
    case QMetaType::QStringList: {
        json arr = json::array();
        for (const QString &s : value.toStringList())
            arr.push_back(s.toStdString());
        return arr;
    }
    default:
        break;
    }

    if (value.canConvert<QVariantList>()) {
        json arr = json::array();
        for (const QVariant &item : value.toList())
            arr.push_back(variantToJson(item));
        return arr;
    }

    if (value.canConvert<QVariantMap>()) {
        json obj = json::object();
        const QVariantMap map = value.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            obj[it.key().toStdString()] = variantToJson(it.value());
        return obj;
    }

    return nullptr;
}

/// nlohmann -> QVariant.
QVariant jsonToVariant(const json &value)
{
    if (value.is_null())        return QVariant();
    if (value.is_boolean())     return value.get<bool>();
    if (value.is_number_integer()) return static_cast<qlonglong>(value.get<qint64>());
    if (value.is_number_unsigned()) return static_cast<qulonglong>(value.get<quint64>());
    if (value.is_number_float()) return value.get<double>();
    if (value.is_string())      return QString::fromStdString(value.get<std::string>());

    if (value.is_array()) {
        QVariantList list;
        list.reserve(static_cast<qsizetype>(value.size()));
        for (const json &item : value)
            list.append(jsonToVariant(item));
        return list;
    }

    if (value.is_object()) {
        QVariantMap map;
        for (const auto &[key, item] : value.items())
            map.insert(QString::fromStdString(key), jsonToVariant(item));
        return map;
    }

    return QVariant();
}

} // namespace

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------

PluginHost::PluginHost(QString pluginId, QString pluginName, QString version,
                       QString dataDir, QObject *parent)
    : QObject(parent)
    , m_pluginId(std::move(pluginId))
    , m_pluginName(std::move(pluginName))
    , m_version(std::move(version))
    , m_dataDir(std::move(dataDir))
{
}

void PluginHost::setAppVersion(const QString &appVersion)
{
    m_appVersion = appVersion;
}

void PluginHost::setDark(bool dark)
{
    if (m_isDark == dark)
        return;
    m_isDark = dark;
    emit themeChanged();
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

bool PluginHost::loadSettings()
{
    if (m_settingsLoaded)
        return true;
    m_settingsLoaded = true;

    QFile file(settingsFileOf(m_dataDir));
    if (!file.exists())
        return true;

    if (!file.open(QIODevice::ReadOnly)) {
        Logger::get().warn("PluginHost[{}]: cannot read settings — {}",
                           m_pluginId.toStdString(), file.errorString().toStdString());
        return false;
    }

    const QByteArray raw = file.readAll();
    file.close();

    try {
        const json root = json::parse(raw.toStdString());
        if (root.is_object()) {
            for (const auto &[key, value] : root.items())
                m_settings.insert(QString::fromStdString(key), jsonToVariant(value));
        }
    } catch (const std::exception &e) {
        // A broken settings file must not stop the plugin from loading; start fresh.
        Logger::get().warn("PluginHost[{}]: settings parse error — {}",
                           m_pluginId.toStdString(), QString::fromUtf8(e.what()).toStdString());
        m_settings.clear();
        return false;
    }

    return true;
}

bool PluginHost::saveSettings() const
{
    if (!QDir().mkpath(m_dataDir)) {
        Logger::get().warn("PluginHost[{}]: cannot create data dir {}",
                           m_pluginId.toStdString(), m_dataDir.toStdString());
        return false;
    }

    json root = json::object();
    for (auto it = m_settings.constBegin(); it != m_settings.constEnd(); ++it)
        root[it.key().toStdString()] = variantToJson(it.value());

    QSaveFile file(settingsFileOf(m_dataDir));
    if (!file.open(QIODevice::WriteOnly)) {
        Logger::get().warn("PluginHost[{}]: cannot write settings — {}",
                           m_pluginId.toStdString(), file.errorString().toStdString());
        return false;
    }

    const QByteArray bytes = QByteArray::fromStdString(root.dump(2));
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }

    return file.commit();
}

QVariant PluginHost::setting(const QString &key, const QVariant &fallback) const
{
    auto *self = const_cast<PluginHost *>(this);
    self->loadSettings();
    return m_settings.value(key, fallback);
}

bool PluginHost::setSetting(const QString &key, const QVariant &value)
{
    loadSettings();

    const bool existed = m_settings.contains(key);
    const QVariant previous = m_settings.value(key);

    m_settings.insert(key, value);

    if (!saveSettings()) {
        // Same contract as PluginRegistry: a failed write leaves memory untouched.
        if (existed)
            m_settings.insert(key, previous);
        else
            m_settings.remove(key);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// log / toast / openPath
// ---------------------------------------------------------------------------

void PluginHost::log(const QString &level, const QString &message)
{
    const std::string tag = "plugin[" + m_pluginId.toStdString() + "]";
    const std::string text = message.toStdString();

    if (level == QLatin1String("debug"))      Logger::get().debug("{}: {}", tag, text);
    else if (level == QLatin1String("warn"))  Logger::get().warn("{}: {}", tag, text);
    else if (level == QLatin1String("error")) Logger::get().error("{}: {}", tag, text);
    else                                      Logger::get().info("{}: {}", tag, text);
}

void PluginHost::toast(const QString &text)
{
    emit toastRequested(text);
}

bool PluginHost::openPath(const QString &path)
{
    if (path.isEmpty())
        return false;

    const QUrl url(path);

    // Remote / mail URLs are always fine.
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("https") || scheme == QLatin1String("http")
        || scheme == QLatin1String("mailto")) {
        return QDesktopServices::openUrl(url);
    }

    // Local paths are confined to the plugin's own data directory. A plugin that
    // wants to open arbitrary files should ask the user through the host UI.
    const QString target = QFileInfo(path).canonicalFilePath();
    const QString allowed = QFileInfo(m_dataDir).canonicalFilePath();

    if (target.isEmpty() || allowed.isEmpty()) {
        Logger::get().warn("PluginHost[{}]: openPath refused (unresolvable) — {}",
                           m_pluginId.toStdString(), path.toStdString());
        return false;
    }

    if (!target.startsWith(allowed, Qt::CaseInsensitive)) {
        Logger::get().warn("PluginHost[{}]: openPath refused (outside data dir) — {}",
                           m_pluginId.toStdString(), path.toStdString());
        return false;
    }

    return QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}

// ---------------------------------------------------------------------------
// 扩展点
// ---------------------------------------------------------------------------

void PluginHost::setExtensionRegistry(PluginExtensionRegistry *registry)
{
    m_extensions = registry;
}

QVariantList PluginHost::extensionPoints() const
{
    if (!m_extensions)
        return {};

    // Only the metadata: a plugin does not get to see other plugins' contributions.
    return m_extensions->points();
}

QObject *PluginHost::addExtension(const QString &pointId, const QVariantMap &data)
{
    setLastError(QString());

    if (!m_extensions) {
        setLastError(QStringLiteral("宿主未提供扩展点"));
        return nullptr;
    }

    QString error;
    PluginDisposable *handle = m_extensions->add(m_pluginId, pointId, data,
                                                 m_disposal, &error);
    if (!handle) {
        setLastError(error.isEmpty() ? QStringLiteral("扩展点注册失败") : error);
        Logger::get().warn("plugin[{}]: addExtension refused — {}",
                           m_pluginId.toStdString(), m_lastError.toStdString());
    }

    return handle;
}

int PluginHost::clearExtensions(const QString &pointId)
{
    if (!m_extensions)
        return 0;

    return m_extensions->removeContributionsOf(m_pluginId, pointId);
}

// ---------------------------------------------------------------------------
// 服务
// ---------------------------------------------------------------------------

void PluginHost::setServiceRegistry(PluginServiceRegistry *registry)
{
    m_services = registry;
}

void PluginHost::setDisposalGroup(DisposalGroup *group)
{
    m_disposal = group;
}

void PluginHost::setAvailableServices(const QStringList &names)
{
    m_availableServices = names;
}

void PluginHost::setManifest(const PluginManifest &manifest)
{
    m_manifest = manifest;

    // Keep the identity fields in sync even when the loader built the host from the
    // same values — one source, no chance of drift.
    if (!manifest.id.isEmpty())
        m_pluginId = manifest.id;
    if (!manifest.name.isEmpty())
        m_pluginName = manifest.name;
    if (!manifest.version.isEmpty())
        m_version = manifest.version;
}

QObject *PluginHost::service(const QString &name)
{
    setLastError(QString());

    if (!m_availableServices.contains(name)) {
        setLastError(QStringLiteral("本插件不可用服务：%1").arg(name));
        return nullptr;
    }

    if (m_serviceViews.contains(name)) {
        QObject *cached = m_serviceViews.value(name).data();
        if (cached)
            return cached;
        m_serviceViews.remove(name);
    }

    if (!m_services) {
        setLastError(QStringLiteral("宿主未提供服务"));
        return nullptr;
    }

    QObject *view = m_services->create(name, this, m_manifest);
    if (!view) {
        setLastError(QStringLiteral("服务不可用：%1").arg(name));
        return nullptr;
    }

    view->setParent(this);
    m_serviceViews.insert(name, view);
    return view;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

QObject *PluginHost::onDispose(QJSValue callback)
{
    setLastError(QString());

    if (!m_disposal) {
        setLastError(QStringLiteral("宿主未提供生命周期组"));
        return nullptr;
    }

    if (!callback.isCallable()) {
        setLastError(QStringLiteral("onDispose 需要一个函数"));
        return nullptr;
    }

    // Copy the value: the callback belongs to the plugin's engine and must stay
    // callable until disposal, which happens before the object is destroyed.
    return m_disposal->add([callback]() mutable {
        callback.call();
    });
}

void PluginHost::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;

    m_lastError = message;
    emit lastErrorChanged();
}
