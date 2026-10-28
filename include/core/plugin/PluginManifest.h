#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

/// `plugin.json` — the manifest that ships inside a plugin package.
///
/// Parsing and validation are deliberately strict and all in one place: a manifest
/// that does not validate must never reach PluginRegistry or PluginLoader, because
/// a half-valid manifest is far more expensive to diagnose later.
///
/// Field reference: docs/architecture/插件市场功能设计.md §5.1.

/// One entry of `contributes.pages` — becomes a row in the left navigation.
struct PageContribution
{
    QString id;         ///< unique within this plugin
    QString title;      ///< navigation label
    QString icon;       ///< emoji or single char; optional
    QString source;     ///< plugin-relative QML path, always normalised to '/'
};

/// One entry of `contributes.searchProviders` — reserved for the A7 search-API work.
struct ProviderContribution
{
    QString id;
    QString title;
    QString trigger;    ///< optional prefix such as "w "; empty means "no trigger"
    int     priority = 100;
};

/// One entry of `contributes.commands` — becomes a searchable command.
struct CommandContribution
{
    QString id;
    QString title;
    QString keyword;    ///< optional keyword used for prefix matching
};

struct PluginManifest
{
    int     manifestVersion = 1;
    QString id;
    QString name;
    QString version;
    QString description;
    QString author;
    QString homepage;
    QString license;
    QString icon;

    /// "qml" (default) or "native". `native` is parsed but rejected during install
    /// in v1 — see LLD D1. Keeping the field here means no schema change later.
    QString runtime = QStringLiteral("qml");

    /// Entry QML file, plugin-relative. Required when runtime == "qml".
    QString main;

    /// Raw `engines.assistant` range, kept verbatim; interpreted by checkEngine().
    /// Empty means "does not declare compatibility" (not "incompatible").
    QString enginesAssistant;

    QStringList permissions;

    QList<PageContribution>     pages;
    QList<ProviderContribution> providers;
    QList<CommandContribution>  commands;

    /// `contributes.settings.source`; parsed but unused in v1.
    QString settingsSource;

    bool isNativeRuntime() const { return runtime == QLatin1String("native"); }

    /// Parse and hard-validate a manifest.
    /// @param error optional human-readable reason on failure (goes into problem.detail)
    /// @return std::nullopt when anything is invalid — never a partially-filled struct
    static std::optional<PluginManifest> parse(const QByteArray &json, QString *error);

    /// Check `engines.assistant` against the running application version.
    /// Returns true when the range is absent (caller should warn, not fail).
    /// @param error on mismatch, a ready-to-show message
    bool checkEngine(const QString &appVersion, QString *error) const;
};
