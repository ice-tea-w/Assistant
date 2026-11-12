#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

class DownloadsDatabase;
class IngestEngine;
class TrackingEngine;
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
    /// 「重新定位」需要追踪引擎的离线反查（级别 1/2）。
    void setTrackingEngine(TrackingEngine *engine) { m_tracking = engine; }

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
    /// S11：撤销本应用的归档（用户手动移走的不动）。
    Q_INVOKABLE bool unarchiveItem(int id);
    /// S11 查重裁决：保留两份（清除 dup 标记）/ 打开更早的那份。
    Q_INVOKABLE bool keepBoth(int id);
    Q_INVOKABLE void revealDuplicate(int id);
    /// 删除重复副本前的 fullSig 裁决；内容不完全一致时返回 false 并设置 lastError。
    Q_INVOKABLE bool prepareDeleteDuplicate(int id);
    /// 把已确认重复的副本移入回收站（只该 id 本身，原文件不动）。
    Q_INVOKABLE bool deleteDuplicate(int id);
    /// 规则 dry-run：不入库、不移动，只返回“当前待整理会命中哪条规则、去哪里”。
    Q_INVOKABLE QVariantList previewRules(const QVariantList &rules) const;
    /// 扫描常见 IM 目录（只返回存在的），供设置页填入后由用户确认。
    Q_INVOKABLE QStringList imCandidates() const;
    /// S11：把当前路径加入待办。
    Q_INVOKABLE bool addToTodo(int id);
    /// S11：读取文本内容到系统剪贴板 + 剪贴板历史（>1 MB 或二进制拒绝）。
    Q_INVOKABLE bool copyContent(int id);
    Q_INVOKABLE bool copyPath(int id);
    /// 手动重新定位一条 Unknown 记录（离线级别 1/2）。
    Q_INVOKABLE bool relocateItem(int id);
    /// 用户确认“已删除”（不自动判定，只改记录）。
    Q_INVOKABLE bool markDeleted(int id);

signals:
    void itemsChanged();
    void lastErrorChanged();

private:
    QVariantList buildItems(const QString &filter) const;
    QString pathOf(int id, bool *inBox) const;

    DownloadsDatabase *m_db = nullptr;
    IngestEngine      *m_engine = nullptr;
    TrackingEngine    *m_tracking = nullptr;
    TodoDatabase      *m_todoDb = nullptr;
    ClipboardDatabase *m_clipDb = nullptr;
    QString m_lastError;
};
