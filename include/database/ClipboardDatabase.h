#pragma once

#include <QObject>
#include <QVariantList>

class QSqlDatabase;

/// @brief SQLite-backed clipboard history (Phase 3 剪贴板).
///
/// Single table `clipboard_history`. Independent connection name "clipboard"
/// and file clipboard.db.
class ClipboardDatabase : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardDatabase(QObject *parent = nullptr);
    ~ClipboardDatabase() override;

    bool open(const QString &path);
    bool isOpen() const;

    /// Append a clip. Consecutive identical content is de-duplicated (returns -1).
    int addClip(const QString &content, const QString &mimeType);

    void deleteClip(int id);
    void clearAll();

    /// Recent clips as QVariantMap list (keys: id, content, mimeType, createdAt),
    /// newest first, capped at @p limit.
    QVariantList listClips(int limit = 200);

    /// Raw text content for a clip id (used by PanelBridge::copyClip). Empty if missing.
    QString clipContent(int id) const;

private:
    void createSchema();

    QSqlDatabase *m_db = nullptr;
    QString m_connectionName;
};
