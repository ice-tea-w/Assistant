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
};
