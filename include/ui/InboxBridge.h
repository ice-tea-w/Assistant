#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

class DownloadsDatabase;
class IngestEngine;
class TodoDatabase;
class ClipboardDatabase;

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

    /// 联动既有功能（S11）：加入待办 / 复制内容需要这两个库。
    void setTodoDatabase(TodoDatabase *db) { m_todoDb = db; }
    void setClipboardDatabase(ClipboardDatabase *db) { m_clipDb = db; }

    QVariantList items() const;
    Q_INVOKABLE QVariantList filterItems(const QString &filter) const;   ///< all / inbox / filed
    int pendingCount() const;
    QString lastError() const { return m_lastError; }

    Q_INVOKABLE void refresh();

    Q_INVOKABLE void openItem(int id);
    Q_INVOKABLE void revealItem(int id);
    Q_INVOKABLE bool undoItem(int id);

    /// S11：按规则建议（suggest_dest）归档。
    Q_INVOKABLE bool archiveItem(int id);
    /// S11：把当前路径加入待办。
    Q_INVOKABLE bool addToTodo(int id);
    /// S11：读取文本内容到系统剪贴板 + 剪贴板历史（>1 MB 或二进制拒绝）。
    Q_INVOKABLE bool copyContent(int id);
    Q_INVOKABLE bool copyPath(int id);

signals:
    void itemsChanged();
    void lastErrorChanged();

private:
    QVariantList buildItems(const QString &filter) const;
    QString pathOf(int id, bool *inBox) const;

    DownloadsDatabase *m_db = nullptr;
    IngestEngine      *m_engine = nullptr;
    TodoDatabase      *m_todoDb = nullptr;
    ClipboardDatabase *m_clipDb = nullptr;
    QString m_lastError;
};
