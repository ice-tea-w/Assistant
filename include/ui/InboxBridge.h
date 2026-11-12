#pragma once

#include <QObject>
#include <QVariantList>

class DownloadsDatabase;
class IngestEngine;

/// 「最近下载」页的 QML↔C++ 桥（Phase C）。
///
/// 与 `PanelBridge` 分开：下载/收件箱有自己的 DB 与引擎，不把主面板桥撑成上帝对象。
class InboxBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY itemsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    InboxBridge(DownloadsDatabase *db, IngestEngine *engine, QObject *parent = nullptr);

    QVariantList items() const;
    Q_INVOKABLE QVariantList filterItems(const QString &filter) const;   ///< all / inbox / filed
    int pendingCount() const;
    QString lastError() const { return m_lastError; }

    Q_INVOKABLE void refresh();

    Q_INVOKABLE void openItem(int id);
    Q_INVOKABLE void revealItem(int id);
    Q_INVOKABLE bool undoItem(int id);

signals:
    void itemsChanged();
    void lastErrorChanged();

private:
    QVariantList buildItems(const QString &filter) const;
    QString pathOf(int id, bool *inBox) const;

    DownloadsDatabase *m_db = nullptr;
    IngestEngine      *m_engine = nullptr;
    QString m_lastError;
};
