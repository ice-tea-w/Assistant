#include "core/plugin/PluginExtensionRegistry.h"

#include "core/Logger.h"
#include "core/plugin/PluginDisposable.h"

#include <QSet>

PluginExtensionRegistry::PluginExtensionRegistry(QObject *parent)
    : QObject(parent)
{
}

PluginExtensionRegistry::~PluginExtensionRegistry()
{
    // Disposing here would run plugin code while the engine may already be gone.
    // The loader disposes per-plugin groups before plugins are destroyed; anything
    // left over is simply dropped with the registry.
    m_entries.clear();
}

// ---------------------------------------------------------------------------
// points
// ---------------------------------------------------------------------------

bool PluginExtensionRegistry::declare(const QString &id, const QString &title,
                                      const QStringList &requiredKeys,
                                      bool consumed, QString *error)
{
    if (id.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("扩展点 id 不能为空");
        return false;
    }

    ExtensionPoint point;
    point.id           = id.trimmed();
    point.title        = title.trimmed();
    point.requiredKeys = requiredKeys;
    point.consumed     = consumed;

    const bool existed = m_points.contains(point.id);
    m_points.insert(point.id, point);

    Logger::get().info("PluginExtensionRegistry: {} extension point {}",
                       existed ? "updated" : "declared", point.id.toStdString());
    return true;
}

bool PluginExtensionRegistry::isDeclared(const QString &id) const
{
    return m_points.contains(id);
}

QStringList PluginExtensionRegistry::pointIds() const
{
    QStringList ids = m_points.keys();
    ids.sort(Qt::CaseSensitive);
    return ids;
}

QVariantList PluginExtensionRegistry::points() const
{
    QVariantList out;

    const QStringList ids = pointIds();      // deterministic order for tests/UI
    for (const QString &id : ids) {
        const ExtensionPoint point = m_points.value(id);
        QVariantMap row;
        row.insert(QStringLiteral("id"), point.id);
        row.insert(QStringLiteral("title"), point.title);
        row.insert(QStringLiteral("consumed"), point.consumed);
        row.insert(QStringLiteral("requiredKeys"), point.requiredKeys);
        out.append(row);
    }

    return out;
}

QString PluginExtensionRegistry::validate(const QString &pointId, const QVariantMap &data) const
{
    if (!m_points.contains(pointId))
        return QStringLiteral("未声明的扩展点：%1").arg(pointId);

    const ExtensionPoint point = m_points.value(pointId);
    for (const QString &key : point.requiredKeys) {
        if (!data.contains(key))
            return QStringLiteral("扩展点 %1 缺少字段 %2").arg(pointId, key);

        const QVariant value = data.value(key);
        if (!value.isValid() || value.isNull())
            return QStringLiteral("扩展点 %1 的字段 %2 为空").arg(pointId, key);

        // Strings must be non-empty; functions/objects (e.g. a command's `run`) are fine.
        if (value.typeId() == QMetaType::QString && value.toString().trimmed().isEmpty())
            return QStringLiteral("扩展点 %1 的字段 %2 为空").arg(pointId, key);
    }

    return QString();
}

// ---------------------------------------------------------------------------
// contributions
// ---------------------------------------------------------------------------

PluginDisposable *PluginExtensionRegistry::add(const QString &pluginId,
                                               const QString &pointId,
                                               const QVariantMap &data,
                                               QString *error)
{
    return addEntry(pluginId, pointId, data, nullptr, error);
}

PluginDisposable *PluginExtensionRegistry::add(const QString &pluginId,
                                               const QString &pointId,
                                               const QVariantMap &data,
                                               DisposalGroup *group,
                                               QString *error)
{
    return addEntry(pluginId, pointId, data, group, error);
}

PluginDisposable *PluginExtensionRegistry::addEntry(const QString &pluginId,
                                                    const QString &pointId,
                                                    const QVariantMap &data,
                                                    DisposalGroup *group,
                                                    QString *error)
{
    if (pluginId.isEmpty()) {
        if (error)
            *error = QStringLiteral("缺少 pluginId");
        return nullptr;
    }

    const QString problem = validate(pointId, data);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return nullptr;
    }

    Entry entry;
    entry.token    = m_nextToken++;
    entry.pluginId = pluginId;
    entry.pointId  = pointId;
    entry.data     = data;

    const int token = entry.token;

    // The disposer is the single removal path: whether the plugin calls dispose()
    // or the loader disposes the whole group, the entry leaves through here.
    auto disposeFn = [this, token]() { removeToken(token); };

    PluginDisposable *handle = group ? group->add(disposeFn)
                                     : new PluginDisposable(pluginId, disposeFn, this);

    entry.handle = handle;
    m_entries.append(entry);

    Logger::get().info("PluginExtensionRegistry: {} contributed to {} (token {})",
                       pluginId.toStdString(), pointId.toStdString(), token);

    emit changed(pointId);
    return handle;
}

void PluginExtensionRegistry::removeToken(int token)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).token != token)
            continue;

        const Entry entry = m_entries.takeAt(i);
        emit changed(entry.pointId);
        return;
    }
}

QVariantList PluginExtensionRegistry::contributions(const QString &pointId) const
{
    QVariantList out;

    for (const Entry &entry : m_entries) {
        if (entry.pointId != pointId)
            continue;

        QVariantMap row = entry.data;
        row.insert(QStringLiteral("pluginId"), entry.pluginId);
        row.insert(QStringLiteral("token"), entry.token);
        out.append(row);
    }

    return out;
}

QVariantList PluginExtensionRegistry::contributionsOf(const QString &pluginId) const
{
    QVariantList out;

    for (const Entry &entry : m_entries) {
        if (entry.pluginId != pluginId)
            continue;

        QVariantMap row = entry.data;
        row.insert(QStringLiteral("pointId"), entry.pointId);
        row.insert(QStringLiteral("token"), entry.token);
        out.append(row);
    }

    return out;
}

int PluginExtensionRegistry::removeAllOf(const QString &pluginId)
{
    // Collect tokens first: disposing mutates m_entries.
    QList<int> tokens;
    for (const Entry &entry : m_entries) {
        if (entry.pluginId == pluginId)
            tokens.append(entry.token);
    }

    int removed = 0;

    for (int token : tokens) {
        PluginDisposable *handle = nullptr;
        for (const Entry &entry : m_entries) {
            if (entry.token == token) {
                handle = entry.handle.data();
                break;
            }
        }

        if (handle && !handle->isDisposed()) {
            // Normal path: the plugin's own handle releases the entry.
            handle->dispose();
            ++removed;
            continue;
        }

        // Handle already gone (group destroyed or plugin disposed it without the
        // registry seeing it) — drop the stale entry directly.
        const int before = m_entries.size();
        removeToken(token);
        if (m_entries.size() < before)
            ++removed;
    }

    return removed;
}

bool PluginExtensionRegistry::removeContribution(int token)
{
    for (const Entry &entry : m_entries) {
        if (entry.token != token)
            continue;

        PluginDisposable *handle = entry.handle.data();
        if (handle && !handle->isDisposed()) {
            handle->dispose();      // goes through removeToken
        } else {
            removeToken(token);
        }
        return true;
    }

    return false;
}

int PluginExtensionRegistry::removeContributionsOf(const QString &pluginId,
                                                   const QString &pointId)
{
    QList<int> tokens;
    for (const Entry &entry : m_entries) {
        if (entry.pluginId == pluginId && entry.pointId == pointId)
            tokens.append(entry.token);
    }

    int removed = 0;
    for (int token : tokens) {
        if (removeContribution(token))
            ++removed;
    }
    return removed;
}

void PluginExtensionRegistry::clear()
{
    QList<int> tokens;
    for (const Entry &entry : m_entries)
        tokens.append(entry.token);

    for (int token : tokens) {
        PluginDisposable *handle = nullptr;
        for (const Entry &entry : m_entries) {
            if (entry.token == token) {
                handle = entry.handle.data();
                break;
            }
        }

        if (handle && !handle->isDisposed())
            handle->dispose();
        else
            removeToken(token);
    }

    m_entries.clear();
}
