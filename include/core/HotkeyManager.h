#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QString>

class HotkeyManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit HotkeyManager(QObject *parent = nullptr);
    ~HotkeyManager() override;

    bool registerHotkey(const QString &modifiers, const QString &key, void *hwnd);

    /// 用**上次注册时的同一个 hwnd** 重新注册（Phase 3 收尾：设置页改热键用）。
    /// 调用方无需自己保管 hwnd。
    bool reRegister(const QString &modifiers, const QString &key);

    void unregisterHotkey();
    bool isRegistered() const;

signals:
    void hotkeyPressed();

protected:
    bool nativeEventFilter(const QByteArray &eventType,
                           void *message, qintptr *result) override;

private:
    static int parseModifiers(const QString &modifiers);
    static int parseKey(const QString &key);

    int  m_hotkeyId = 0;
    bool m_registered = false;
    /// 注册时用的窗口句柄——unregisterHotkey 必须用**同一个**句柄，
    /// 传 nullptr 会注销不掉（热键与窗口关联，不属于线程队列）。
    void *m_hwnd = nullptr;
};
