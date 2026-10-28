#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

class PluginManager;

/// QML <-> C++ adapter for the plugin market.
///
/// Deliberately **zero business logic** (插件市场-模块框架.md §4.7): it neither decides
/// whether something can be installed nor touches files or the network. Every call
/// forwards to PluginManager; every signal forwards back.
///
/// The property shapes here are what PluginsPage.qml binds to, and they follow the
/// project convention used by TodoPage/NotesPage: a plain QVariantList bound as
/// `model`, consumed with `modelData.xxx`. No QAbstractListModel subclass — that
/// would be the only one of its kind in the codebase.
///
/// Exposed to QML as the context property `plugins`.
class PluginBridge : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList installedList   READ installedList   NOTIFY listsChanged)
    Q_PROPERTY(QVariantList availableList   READ availableList   NOTIFY listsChanged)
    Q_PROPERTY(QVariantList categories      READ categories      NOTIFY listsChanged)
    Q_PROPERTY(QVariantList navigationEntries READ navigationEntries NOTIFY navigationChanged)
    Q_PROPERTY(bool         isOffline       READ isOffline       NOTIFY catalogChanged)
    Q_PROPERTY(QString      fetchedAtText   READ fetchedAtText   NOTIFY catalogChanged)
    Q_PROPERTY(bool         loading         READ loading         NOTIFY loadingChanged)

public:
    explicit PluginBridge(PluginManager *manager, QObject *parent = nullptr);

    QVariantList installedList() const;
    QVariantList availableList() const;
    QVariantList categories() const;
    QVariantList navigationEntries() const;
    bool    isOffline() const;
    QString fetchedAtText() const;
    bool    loading() const { return m_loading; }

    // ---- intents (thin pass-through) ----
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void install(const QString &id);

    /// Open the native file picker and side-load the chosen .zip.
    /// A file picker is used instead of QML FileDialog because QtQuick.Dialogs is
    /// not part of the modules this project links; Widgets is.
    Q_INVOKABLE void pickAndInstall();

    Q_INVOKABLE void installFromZip(const QString &zipPath);
    Q_INVOKABLE void cancelInstall(const QString &requestId);
    Q_INVOKABLE void setEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void uninstall(const QString &id);
    Q_INVOKABLE void openPluginDir(const QString &id);

    /// Instantiate (cached) a plugin-contributed page and return the QQuickItem.
    /// Ownership stays in C++; QML reparents it into a slot and binds its size.
    /// @param key "<pluginId>:<pageId>"
    Q_INVOKABLE QObject *acquirePage(const QString &key);

signals:
    void listsChanged();
    void navigationChanged();
    void catalogChanged();
    void loadingChanged();
    void toast(const QString &level, const QString &text);

    // ---- install progress (forwarded from PluginManager) ----
    void installProgress(const QString &requestId, int stage, int percent,
                         const QString &detail);
    void installLog(const QString &requestId, const QString &line);
    void installFinished(const QString &requestId, bool ok, const QString &code,
                         const QString &message);

private:
    PluginManager *m_manager = nullptr;
    bool m_loading = false;
};
