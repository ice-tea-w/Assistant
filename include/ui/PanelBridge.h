#pragma once

#include <QObject>
#include <QVariantList>

class TodoDatabase;
class NotesDatabase;
class ClipboardDatabase;
class ConfigManager;
class HotkeyManager;

/// @brief QML↔C++ bridge for the main panel (Phase 3).
///
/// Separate from AppBridge (search floater) so each window owns its surface.
/// Exposes the three feature lists as Q_PROPERTY and CRUD as Q_INVOKABLE.
class PanelBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList todoList READ todoList NOTIFY todoListChanged)
    Q_PROPERTY(QVariantList notesList READ notesList NOTIFY notesListChanged)
    Q_PROPERTY(QVariantList clipList READ clipList NOTIFY clipListChanged)

public:
    explicit PanelBridge(QObject *parent = nullptr);

    void setTodoDatabase(TodoDatabase *db);
    void setNotesDatabase(NotesDatabase *db);
    void setClipboardDatabase(ClipboardDatabase *db);

    /// 设置页改热键需要（主面板持有这两个对象；AppBridge 不参与）。
    void setConfigManager(ConfigManager *config);
    void setHotkeyManager(HotkeyManager *hotkey);

    /// 把全局热键改为 @p modifiers + @p key（形如 "Ctrl+Alt" / "Space"）。
    /// 成功：写回配置并返回 true；失败（被占用 / 不支持的键）：**恢复原热键**并返回 false。
    Q_INVOKABLE bool applyHotkey(const QString &modifiers, const QString &key);

    /// Start recording text clips from QClipboard::dataChanged.
    void startClipboardMonitor();

    QVariantList todoList() const;
    QVariantList notesList() const;
    QVariantList clipList() const;

    // ---- todo ----
    Q_INVOKABLE void addTodo(const QString &content);
    Q_INVOKABLE void toggleTodo(int id);
    Q_INVOKABLE void deleteTodo(int id);

    // ---- notes ----
    Q_INVOKABLE int addNote(const QString &title, const QString &content);
    Q_INVOKABLE void updateNote(int id, const QString &title, const QString &content);
    Q_INVOKABLE void deleteNote(int id);

    // ---- clipboard ----
    Q_INVOKABLE void copyClip(int id);
    Q_INVOKABLE void deleteClip(int id);
    Q_INVOKABLE void clearClips();

signals:
    void todoListChanged();
    void notesListChanged();
    void clipListChanged();

private:
    TodoDatabase *m_todoDb = nullptr;
    NotesDatabase *m_notesDb = nullptr;
    ClipboardDatabase *m_clipDb = nullptr;
    ConfigManager *m_config = nullptr;
    HotkeyManager *m_hotkey = nullptr;
};
