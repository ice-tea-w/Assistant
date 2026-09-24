#include "ui/PanelBridge.h"

#include "database/TodoDatabase.h"
#include "database/NotesDatabase.h"
#include "database/ClipboardDatabase.h"
#include "core/ConfigManager.h"
#include "core/HotkeyManager.h"
#include "core/Logger.h"

#include <QApplication>
#include <QClipboard>

PanelBridge::PanelBridge(QObject *parent)
    : QObject(parent)
{
}

void PanelBridge::setTodoDatabase(TodoDatabase *db) { m_todoDb = db; }
void PanelBridge::setNotesDatabase(NotesDatabase *db) { m_notesDb = db; }
void PanelBridge::setClipboardDatabase(ClipboardDatabase *db) { m_clipDb = db; }
void PanelBridge::setConfigManager(ConfigManager *config) { m_config = config; }
void PanelBridge::setHotkeyManager(HotkeyManager *hotkey) { m_hotkey = hotkey; }

// ---- hotkey (Phase 3 收尾) ----

bool PanelBridge::applyHotkey(const QString &modifiers, const QString &key)
{
    if (!m_hotkey || !m_config)
        return false;

    const QString oldMods = m_config->hotkeyModifiers();
    const QString oldKey  = m_config->hotkeyKey();

    // 先尝试注册新组合键：registerHotkey 内部会先注销旧的
    if (!m_hotkey->reRegister(modifiers, key)) {
        // 注册失败 → 把旧热键注册回去，绝不留下"没有任何热键"的状态
        m_hotkey->reRegister(oldMods, oldKey);
        Logger::get().warn("PanelBridge: hotkey {}+{} rejected, restored {}+{}",
                           modifiers.toStdString(), key.toStdString(),
                           oldMods.toStdString(), oldKey.toStdString());
        return false;
    }

    // 注册成功才持久化（配置与实际注册状态保持一致）
    m_config->setHotkey(modifiers, key);
    Logger::get().info("PanelBridge: hotkey changed to {}+{}",
                       modifiers.toStdString(), key.toStdString());
    return true;
}

QVariantList PanelBridge::todoList() const
{
    return m_todoDb ? m_todoDb->listTodos() : QVariantList();
}

QVariantList PanelBridge::notesList() const
{
    return m_notesDb ? m_notesDb->listNotes() : QVariantList();
}

QVariantList PanelBridge::clipList() const
{
    return m_clipDb ? m_clipDb->listClips() : QVariantList();
}

// ---- todo ----

void PanelBridge::addTodo(const QString &content)
{
    if (m_todoDb && m_todoDb->addTodo(content) >= 0)
        emit todoListChanged();
}

void PanelBridge::toggleTodo(int id)
{
    if (m_todoDb) {
        m_todoDb->toggleDone(id);
        emit todoListChanged();
    }
}

void PanelBridge::deleteTodo(int id)
{
    if (m_todoDb) {
        m_todoDb->deleteTodo(id);
        emit todoListChanged();
    }
}

// ---- notes ----

int PanelBridge::addNote(const QString &title, const QString &content)
{
    if (!m_notesDb) return -1;

    const int id = m_notesDb->addNote(title, content);
    if (id >= 0)
        emit notesListChanged();
    return id;
}

void PanelBridge::updateNote(int id, const QString &title, const QString &content)
{
    if (m_notesDb) {
        m_notesDb->updateNote(id, title, content);
        emit notesListChanged();
    }
}

void PanelBridge::deleteNote(int id)
{
    if (m_notesDb) {
        m_notesDb->deleteNote(id);
        emit notesListChanged();
    }
}

// ---- clipboard ----

void PanelBridge::copyClip(int id)
{
    if (!m_clipDb) return;

    const QString text = m_clipDb->clipContent(id);
    if (text.isEmpty()) return;

    QClipboard *clipboard = QApplication::clipboard();
    if (clipboard)
        clipboard->setText(text);
}

void PanelBridge::deleteClip(int id)
{
    if (m_clipDb) {
        m_clipDb->deleteClip(id);
        emit clipListChanged();
    }
}

void PanelBridge::clearClips()
{
    if (m_clipDb) {
        m_clipDb->clearAll();
        emit clipListChanged();
    }
}

void PanelBridge::startClipboardMonitor()
{
    QClipboard *clipboard = QApplication::clipboard();
    if (!clipboard) return;

    connect(clipboard, &QClipboard::dataChanged, this, [this]() {
        if (!m_clipDb) return;

        QClipboard *cb = QApplication::clipboard();
        if (!cb) return;

        const QString text = cb->text();
        if (text.isEmpty()) return;

        if (m_clipDb->addClip(text, QStringLiteral("text/plain")) >= 0)
            emit clipListChanged();
    });
}
