#pragma once

#include <QObject>
#include <QVariantList>

#include "ui/SearchResult.h"

class HistoryDatabase;
class FileIndex;

class AppBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchResultsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY visibleChanged)

public:
    explicit AppBridge(QObject *parent = nullptr);

    QVariantList searchResults() const;
    QString statusText() const;
    bool isVisible() const;
    void setVisible(bool visible);

    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void executeCommand(int index);
    Q_INVOKABLE void activate();
    Q_INVOKABLE void hideWindow();
    Q_INVOKABLE void toggleWindow();
    Q_INVOKABLE void openMainPanel();

    /// Set the history database (owned by main; AppBridge does not take ownership).
    void setHistoryDatabase(HistoryDatabase *db);

    /// Set the v2 file index (owned by main; AppBridge does not take ownership).
    ///
    /// 传 nullptr 表示走旧 `FileSearcher`（`fileIndex.engine = "legacy"`）——
    /// A2 的 A/B 开关就落在这里：**两条路都能跑**，A7 才删掉旧的。
    void setFileIndex(FileIndex *index);

    /// Register an indexed application as a searchable/launchable result.
    void addApp(const QString &name, const QString &path);

signals:
    void searchResultsChanged();
    void statusTextChanged();
    void visibleChanged();
    void commandExecuted(const QString &command);
    void mainPanelRequested();

private:
    void executeAction(const SearchResult &item);
    void setStatus(const QString &text);

    /// 文件结果的两种取法：`legacy` 走旧 FileSearcher（A1 语义），
    /// `v2` 走 FileIndex 快照。两条路产出的 SearchResult 字段完全一致。
    QList<SearchResult> legacyFileResults(const QString &query) const;
    QList<SearchResult> fileResults(const QString &query);

    QList<SearchResult> m_allItems;
    QList<SearchResult> m_searchResults;
    QString m_statusText;
    QString m_lastQuery;   // 最近一次查询：执行结果时用它写搜索历史（A1）
    bool m_visible = false;
    HistoryDatabase *m_historyDb = nullptr;
    FileIndex       *m_fileIndex = nullptr;
};
