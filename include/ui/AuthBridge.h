#pragma once

#include <QObject>
#include <QString>

/// 账号与登录状态（本地账号，离线可用）
///
/// - 账号密码只保存在本机 `%LOCALAPPDATA%/AssistantTeam/Assistant/account.json`，
///   口令用 PBKDF2-HMAC-SHA256（随机盐 + 100000 次迭代）存哈希，不存明文；
/// - 登录是本地校验，不需要联网（离线登录）；
/// - 未登录/离线模式下部分功能受限（设备同步、插件市场由 QML 遮罩拦截）。
class AuthBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool hasAccount READ hasAccount NOTIFY changed)
    Q_PROPERTY(bool offline READ offline NOTIFY changed)
    Q_PROPERTY(bool needsLogin READ needsLogin NOTIFY changed)
    Q_PROPERTY(QString userName READ userName NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)

public:
    explicit AuthBridge(QObject *parent = nullptr);

    bool signedIn() const { return m_signedIn; }
    bool hasAccount() const { return m_hasAccount; }
    bool offline() const { return m_offline; }
    bool needsLogin() const { return !m_signedIn && !m_offline; }
    QString userName() const { return m_user; }
    QString statusText() const { return m_status; }

    Q_INVOKABLE void login(const QString &user, const QString &password);
    Q_INVOKABLE void continueOffline();
    Q_INVOKABLE void requestLogin();
    Q_INVOKABLE void saveAccount(const QString &user, const QString &password);
    Q_INVOKABLE void logout();

signals:
    void changed();
    void toast(const QString &text);

private:
    void load();
    void saveSession(bool signedIn) const;
    bool verifyPassword(const QString &user, const QString &password) const;
    void setStatus(const QString &text);

    bool m_signedIn = false;
    bool m_hasAccount = false;
    bool m_offline = false;
    QString m_user;
    QString m_status;
};
