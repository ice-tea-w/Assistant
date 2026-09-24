#include "core/HotkeyManager.h"
#include "core/Logger.h"

#include <windows.h>

static int g_hotkeyIdCounter = 1000;

HotkeyManager::HotkeyManager(QObject *parent)
    : QObject(parent)
{
}

HotkeyManager::~HotkeyManager()
{
    unregisterHotkey();
}

bool HotkeyManager::registerHotkey(const QString &modifiers,
                                    const QString &key,
                                    void *hwnd)
{
    unregisterHotkey();

    int mod = parseModifiers(modifiers);
    int vk  = parseKey(key);

    if (vk == 0) {
        Logger::get().warn("HotkeyManager: unsupported key: {}", key.toStdString());
        return false;
    }

    m_hotkeyId = g_hotkeyIdCounter++;
    m_hwnd = hwnd;                       // 注销时必须用同一个句柄

    BOOL ok = RegisterHotKey(
        reinterpret_cast<HWND>(hwnd),
        m_hotkeyId,
        mod,
        vk
    );

    if (!ok) {
        Logger::get().warn("HotkeyManager: RegisterHotKey failed, error: {}", GetLastError());
        m_hotkeyId = 0;
        return false;
    }

    m_registered = true;
    Logger::get().debug("HotkeyManager: registered {}+{} (id: {})",
                         modifiers.toStdString(), key.toStdString(), m_hotkeyId);
    return true;
}

void HotkeyManager::unregisterHotkey()
{
    if (m_registered) {
        // 用注册时的 hwnd（早前写成 nullptr 会注销失败，残留热键会被系统吞掉）
        UnregisterHotKey(reinterpret_cast<HWND>(m_hwnd), m_hotkeyId);
        m_registered = false;
        m_hotkeyId = 0;
    }
}

bool HotkeyManager::reRegister(const QString &modifiers, const QString &key)
{
    return registerHotkey(modifiers, key, m_hwnd);
}

bool HotkeyManager::isRegistered() const
{
    return m_registered;
}

bool HotkeyManager::nativeEventFilter(const QByteArray &eventType,
                                       void *message, qintptr *result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)

    MSG *msg = static_cast<MSG *>(message);
    if (msg->message == WM_HOTKEY
        && msg->wParam == static_cast<WPARAM>(m_hotkeyId)) {
        emit hotkeyPressed();
        return true;
    }
    return false;
}

int HotkeyManager::parseModifiers(const QString &modifiers)
{
    int mod = 0;
    const QStringList parts = modifiers.split('+', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        QString t = part.trimmed().toLower();
        if      (t == "alt")   mod |= MOD_ALT;
        else if (t == "ctrl")  mod |= MOD_CONTROL;
        else if (t == "shift") mod |= MOD_SHIFT;
        else if (t == "win")   mod |= MOD_WIN;
    }
    return (mod == 0) ? MOD_ALT : mod;
}

int HotkeyManager::parseKey(const QString &key)
{
    QString k = key.trimmed();

    // F1-F24
    if (k.length() >= 2 && k[0].toUpper() == QChar('F')) {
        bool ok = false;
        int num = k.mid(1).toInt(&ok);
        if (ok && num >= 1 && num <= 24)
            return VK_F1 + num - 1;
    }

    // A-Z, 0-9
    if (k.length() == 1) {
        QChar c = k[0];
        if (c.isLetter())
            return c.toUpper().unicode();
        if (c.isDigit())
            return c.unicode();
    }

    // named keys
    QString lower = k.toLower();
    if (lower == "space")  return VK_SPACE;
    if (lower == "escape" || lower == "esc") return VK_ESCAPE;
    if (lower == "tab")    return VK_TAB;
    if (lower == "return" || lower == "enter") return VK_RETURN;

    Logger::get().warn("HotkeyManager: unknown key: {}", key.toStdString());
    return 0;
}
