#pragma once

#include <QObject>
#include <QVariantList>

class QSqlDatabase;

/// @brief SQLite-backed todo list (Phase 3 待办事项).
///
/// Single table `todos`. Independent connection name "todo" and file todo.db,
/// mirroring the HistoryDatabase pattern (Qt6::Sql + WAL mode).
class TodoDatabase : public QObject
{
    Q_OBJECT

public:
    explicit TodoDatabase(QObject *parent = nullptr);
    ~TodoDatabase() override;

    bool open(const QString &path);
    bool isOpen() const;

    /// Insert one un-done todo; returns new id, or -1 on failure/empty content.
    int addTodo(const QString &content);

    /// Flip done; when completing, stamp completed_at; when un-completing, clear it.
    void toggleDone(int id);

    void deleteTodo(int id);

    /// All todos as QVariantMap list (keys: id, content, done, createdAt, completedAt).
    /// Un-done first (created asc), done sunk to bottom (completed desc).
    QVariantList listTodos();

private:
    void createSchema();

    QSqlDatabase *m_db = nullptr;
    QString m_connectionName;
};
