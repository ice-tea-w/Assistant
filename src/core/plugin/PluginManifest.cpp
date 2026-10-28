#include "core/plugin/PluginManifest.h"

#include "core/plugin/SemVer.h"

#include <QRegularExpression>
#include <QStringList>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

/// Plugin ids are reverse-DNS-ish but restricted to a safe character set so that
/// the id can be used directly as a directory name on every platform.
const QRegularExpression &idPattern()
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9._-]{2,63}$"));
    return re;
}

/// Reject anything that could escape the plugin directory.
///
/// Checks are applied to a '/'-normalised copy, so "a\\..\\b" is caught just like
/// "a/../b". Absolute paths (POSIX style and drive letters) are refused as well,
/// because a manifest must never be able to point outside its own package.
bool isSafeRelativePath(const QString &raw, QString *why)
{
    QString p = raw;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    p = p.trimmed();

    if (p.isEmpty()) {
        *why = QStringLiteral("路径为空");
        return false;
    }

    if (p.startsWith(QLatin1Char('/'))) {
        *why = QStringLiteral("不能是绝对路径");
        return false;
    }

    if (p.size() >= 2 && p.at(1) == QLatin1Char(':') && p.at(0).isLetter()) {
        *why = QStringLiteral("不能带盘符");
        return false;
    }

    const QStringList segments = p.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (segment == QLatin1String("..")) {
            *why = QStringLiteral("不能包含 ..");
            return false;
        }
    }

    return true;
}

/// Read an optional string member, reporting a type error instead of throwing.
bool readString(const json &obj, const char *key, QString *out, bool *present, QString *error)
{
    *present = false;
    if (!obj.contains(key))
        return true;

    const json &value = obj.at(key);
    if (!value.is_string()) {
        *error = QStringLiteral("字段 %1 应为字符串").arg(QLatin1String(key));
        return false;
    }

    *out = QString::fromStdString(value.get<std::string>());
    *present = true;
    return true;
}

/// Read an optional integer member.
bool readInt(const json &obj, const char *key, int *out, bool *present, QString *error)
{
    *present = false;
    if (!obj.contains(key))
        return true;

    const json &value = obj.at(key);
    if (!value.is_number_integer()) {
        *error = QStringLiteral("字段 %1 应为整数").arg(QLatin1String(key));
        return false;
    }

    *out = value.get<int>();
    *present = true;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// parse
// ---------------------------------------------------------------------------

std::optional<PluginManifest> PluginManifest::parse(const QByteArray &raw, QString *error)
{
    const auto fail = [error](const QString &message) -> std::optional<PluginManifest> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    json root;
    try {
        root = json::parse(raw.toStdString());
    } catch (const std::exception &e) {
        return fail(QStringLiteral("不是合法的 JSON：%1").arg(QString::fromUtf8(e.what())));
    }

    if (!root.is_object())
        return fail(QStringLiteral("根节点应为对象"));

    PluginManifest m;

    // ---- manifestVersion ----
    {
        bool present = false;
        if (!readInt(root, "manifestVersion", &m.manifestVersion, &present, error))
            return std::nullopt;
        if (!present)
            return fail(QStringLiteral("缺少字段 manifestVersion"));
        if (m.manifestVersion != 1)
            return fail(QStringLiteral("不支持的 manifestVersion：%1（本版本只认 1）")
                            .arg(m.manifestVersion));
    }

    // ---- id ----
    {
        bool present = false;
        if (!readString(root, "id", &m.id, &present, error))
            return std::nullopt;
        if (!present)
            return fail(QStringLiteral("缺少字段 id"));
        if (!idPattern().match(m.id).hasMatch())
            return fail(QStringLiteral("id 非法：%1（需 ^[a-z0-9][a-z0-9._-]{2,63}$）").arg(m.id));
    }

    // ---- name ----
    {
        bool present = false;
        if (!readString(root, "name", &m.name, &present, error))
            return std::nullopt;
        if (!present || m.name.trimmed().isEmpty())
            return fail(QStringLiteral("缺少字段 name"));
    }

    // ---- version ----
    {
        bool present = false;
        if (!readString(root, "version", &m.version, &present, error))
            return std::nullopt;
        if (!present)
            return fail(QStringLiteral("缺少字段 version"));
        if (!semver::parse(QStringView(m.version)))
            return fail(QStringLiteral("version 不是合法的语义化版本：%1").arg(m.version));
    }

    // ---- optional scalars ----
    bool dummyString = false;
    if (!readString(root, "description", &m.description, &dummyString, error)) return std::nullopt;
    if (!readString(root, "author",      &m.author,      &dummyString, error)) return std::nullopt;
    if (!readString(root, "homepage",    &m.homepage,    &dummyString, error)) return std::nullopt;
    if (!readString(root, "license",     &m.license,     &dummyString, error)) return std::nullopt;
    if (!readString(root, "icon",        &m.icon,        &dummyString, error)) return std::nullopt;

    // ---- runtime ----
    {
        bool present = false;
        if (!readString(root, "runtime", &m.runtime, &present, error))
            return std::nullopt;
        if (!present || m.runtime.isEmpty())
            m.runtime = QStringLiteral("qml");
        if (m.runtime != QLatin1String("qml") && m.runtime != QLatin1String("native"))
            return fail(QStringLiteral("runtime 只能是 qml 或 native，收到：%1").arg(m.runtime));
    }

    // ---- main (required for qml plugins) ----
    {
        bool present = false;
        if (!readString(root, "main", &m.main, &present, error))
            return std::nullopt;

        if (!present || m.main.isEmpty()) {
            if (!m.isNativeRuntime())
                return fail(QStringLiteral("runtime 为 qml 时必须提供 main"));
        } else {
            QString why;
            if (!isSafeRelativePath(m.main, &why))
                return fail(QStringLiteral("main 路径非法（%1）：%2").arg(why, m.main));
            m.main.replace(QLatin1Char('\\'), QLatin1Char('/'));
        }
    }

    // ---- engines.assistant ----
    if (root.contains("engines")) {
        const json &engines = root.at("engines");
        if (!engines.is_object())
            return fail(QStringLiteral("字段 engines 应为对象"));

        bool present = false;
        if (!readString(engines, "assistant", &m.enginesAssistant, &present, error))
            return std::nullopt;

        if (present && !m.enginesAssistant.isEmpty()) {
            QString why;
            if (!semver::isValidRange(QStringView(m.enginesAssistant), &why))
                return fail(QStringLiteral("engines.assistant 无法解析（%1）：%2")
                                .arg(why, m.enginesAssistant));
        }
    }

    // ---- permissions ----
    if (root.contains("permissions")) {
        const json &perms = root.at("permissions");
        if (!perms.is_array())
            return fail(QStringLiteral("字段 permissions 应为数组"));
        for (const json &item : perms) {
            if (!item.is_string())
                return fail(QStringLiteral("permissions 里应为字符串"));
            m.permissions.append(QString::fromStdString(item.get<std::string>()));
        }
    }

    // ---- contributes ----
    if (root.contains("contributes")) {
        const json &contributes = root.at("contributes");
        if (!contributes.is_object())
            return fail(QStringLiteral("字段 contributes 应为对象"));

        // pages
        if (contributes.contains("pages")) {
            const json &pages = contributes.at("pages");
            if (!pages.is_array())
                return fail(QStringLiteral("contributes.pages 应为数组"));

            QStringList seenIds;
            for (const json &item : pages) {
                if (!item.is_object())
                    return fail(QStringLiteral("contributes.pages 的每一项应为对象"));

                PageContribution page;
                bool present = false;

                if (!readString(item, "id", &page.id, &present, error)) return std::nullopt;
                if (!present || page.id.trimmed().isEmpty())
                    return fail(QStringLiteral("pages 项缺少 id"));
                if (seenIds.contains(page.id))
                    return fail(QStringLiteral("pages 里的 id 重复：%1").arg(page.id));
                seenIds.append(page.id);

                if (!readString(item, "title", &page.title, &present, error)) return std::nullopt;
                if (!present || page.title.trimmed().isEmpty())
                    return fail(QStringLiteral("pages 项缺少 title"));

                if (!readString(item, "icon", &page.icon, &present, error)) return std::nullopt;

                if (!readString(item, "source", &page.source, &present, error)) return std::nullopt;
                if (!present || page.source.isEmpty())
                    return fail(QStringLiteral("pages 项缺少 source"));

                QString why;
                if (!isSafeRelativePath(page.source, &why))
                    return fail(QStringLiteral("pages 项 %1 的 source 非法（%2）：%3")
                                    .arg(page.id, why, page.source));
                page.source.replace(QLatin1Char('\\'), QLatin1Char('/'));

                m.pages.append(page);
            }
        }

        // searchProviders
        if (contributes.contains("searchProviders")) {
            const json &providers = contributes.at("searchProviders");
            if (!providers.is_array())
                return fail(QStringLiteral("contributes.searchProviders 应为数组"));

            QStringList seenIds;
            for (const json &item : providers) {
                if (!item.is_object())
                    return fail(QStringLiteral("searchProviders 的每一项应为对象"));

                ProviderContribution provider;
                bool present = false;

                if (!readString(item, "id", &provider.id, &present, error)) return std::nullopt;
                if (!present || provider.id.trimmed().isEmpty())
                    return fail(QStringLiteral("searchProviders 项缺少 id"));
                if (seenIds.contains(provider.id))
                    return fail(QStringLiteral("searchProviders 里的 id 重复：%1").arg(provider.id));
                seenIds.append(provider.id);

                if (!readString(item, "title", &provider.title, &present, error)) return std::nullopt;
                if (!present || provider.title.trimmed().isEmpty())
                    return fail(QStringLiteral("searchProviders 项缺少 title"));

                if (!readString(item, "trigger", &provider.trigger, &present, error))
                    return std::nullopt;

                if (!readInt(item, "priority", &provider.priority, &present, error))
                    return std::nullopt;

                m.providers.append(provider);
            }
        }

        // commands
        if (contributes.contains("commands")) {
            const json &commands = contributes.at("commands");
            if (!commands.is_array())
                return fail(QStringLiteral("contributes.commands 应为数组"));

            QStringList seenIds;
            for (const json &item : commands) {
                if (!item.is_object())
                    return fail(QStringLiteral("commands 的每一项应为对象"));

                CommandContribution command;
                bool present = false;

                if (!readString(item, "id", &command.id, &present, error)) return std::nullopt;
                if (!present || command.id.trimmed().isEmpty())
                    return fail(QStringLiteral("commands 项缺少 id"));
                if (seenIds.contains(command.id))
                    return fail(QStringLiteral("commands 里的 id 重复：%1").arg(command.id));
                seenIds.append(command.id);

                if (!readString(item, "title", &command.title, &present, error)) return std::nullopt;
                if (!present || command.title.trimmed().isEmpty())
                    return fail(QStringLiteral("commands 项缺少 title"));

                if (!readString(item, "keyword", &command.keyword, &present, error)) return std::nullopt;

                m.commands.append(command);
            }
        }

        // settings
        if (contributes.contains("settings")) {
            const json &settings = contributes.at("settings");
            if (!settings.is_object())
                return fail(QStringLiteral("contributes.settings 应为对象"));

            bool present = false;
            if (!readString(settings, "source", &m.settingsSource, &present, error))
                return std::nullopt;

            if (present && !m.settingsSource.isEmpty()) {
                QString why;
                if (!isSafeRelativePath(m.settingsSource, &why))
                    return fail(QStringLiteral("settings.source 非法（%1）：%2")
                                    .arg(why, m.settingsSource));
                m.settingsSource.replace(QLatin1Char('\\'), QLatin1Char('/'));
            }
        }
    }

    return m;
}

// ---------------------------------------------------------------------------
// checkEngine
// ---------------------------------------------------------------------------

bool PluginManifest::checkEngine(const QString &appVersion, QString *error) const
{
    // Missing range means "does not declare compatibility", which is not a failure.
    // The caller is expected to surface a warning instead.
    if (enginesAssistant.isEmpty())
        return true;

    const auto app = semver::parse(QStringView(appVersion));
    if (!app) {
        if (error) {
            *error = QStringLiteral("宿主版本号无法解析：%1").arg(appVersion);
        }
        return false;
    }

    if (!semver::satisfies(*app, QStringView(enginesAssistant))) {
        if (error) {
            *error = QStringLiteral("该插件要求 Assistant %1，当前 %2")
                         .arg(enginesAssistant, appVersion);
        }
        return false;
    }

    return true;
}
