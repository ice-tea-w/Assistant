#pragma once

#include <QObject>
#include <QVariantList>

class QSqlDatabase;

/// @brief SQLite-backed notes (Phase 3 记事本).
///
/// Single table `notes`. Independent connection name "notes" and file notes.db.
class NotesDatabase : public QObject
{
    Q_OBJECT

public:
    explicit NotesDatabase(QObject *parent = nullptr);
    ~NotesDatabase() override;

    bool open(const QString &path);
    bool isOpen() const;

    /// Create a note, returning its id.
    int addNote(const QString &title, const QString &content);

    /// Update a note and refresh updated_at.
    void updateNote(int id, const QString &title, const QString &content);

    void deleteNote(int id);

    /// All notes as QVariantMap list (keys: id, title, content, createdAt, updatedAt),
    /// most recently edited first.
    QVariantList listNotes();

private:
    void createSchema();

    QSqlDatabase *m_db = nullptr;
    QString m_connectionName;
};
