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
    Q_INVOKABLE void installFromZip(const QString &zipPath);
    Q_INVOKABLE void cancelInstall(const QString &requestId);
    Q_INVOKABLE void setEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void uninstall(const QString &id);
    Q_INVOKABLE void openPluginDir(const QString &id);

signals:
    void listsChanged();
    void navigationChanged();
    void catalogChanged();
    void loadingChanged();
    void toast(const QString &level, const QString &text);

private:
    PluginManager *m_manager = nullptr;
    bool m_loading = false;
};
