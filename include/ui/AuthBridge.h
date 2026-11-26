#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

/// 账号与登录状态（本地账号，离线可用）
///
/// 安全设计：
/// - 凭据文件放在本机 `%LOCALAPPDATA%/AssistantTeam/Assistant/.auth/account.dat`，
///   整个 JSON 用 Windows DPAPI（当前用户）加密，文件里没有明文口令；
/// - 口令用 PBKDF2-HMAC-SHA256（随机盐 + 100000 次迭代）存哈希；
/// - 登录/校验均在本地完成，不需要联网（离线登录）；
/// - 界面只显示状态，不展示存储路径。
class AuthBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool hasAccount READ hasAccount NOTIFY changed)
    Q_PROPERTY(bool offline READ offline NOTIFY changed)
    Q_PROPERTY(bool needsLogin READ needsLogin NOTIFY changed)
    Q_PROPERTY(QString userName READ userName NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(bool loginLocked READ loginLocked NOTIFY changed)
    Q_PROPERTY(int lockRemainingSeconds READ lockRemainingSeconds NOTIFY changed)

public:
    explicit AuthBridge(QObject *parent = nullptr);

    bool signedIn() const { return m_signedIn; }
    bool hasAccount() const { return m_hasAccount; }
    bool offline() const { return m_offline; }
    bool needsLogin() const { return !m_signedIn && !m_offline; }
    QString userName() const { return m_user; }
    QString statusText() const { return m_status; }
    bool loginLocked() const { return lockRemainingMs() > 0; }
    int lockRemainingSeconds() const;

    Q_INVOKABLE void login(const QString &user, const QString &password);
    Q_INVOKABLE void continueOffline();
    Q_INVOKABLE void requestLogin();
    Q_INVOKABLE void saveAccount(const QString &user, const QString &password);
    Q_INVOKABLE void changePassword(const QString &oldPassword, const QString &newPassword);
    Q_INVOKABLE void changeUserName(const QString &newName);
    Q_INVOKABLE void switchAccount();
    Q_INVOKABLE void logout();

signals:
    void changed();
    void toast(const QString &text);

private:
    void load();
    void saveSession(bool signedIn);
    bool verifyPassword(const QString &user, const QString &password) const;
    void setStatus(const QString &text);
    qint64 lockRemainingMs() const;
    void updateSecurityState(int failedAttempts, qint64 lockUntilMs);
    void startLockTimer();

    bool m_signedIn = false;
    bool m_hasAccount = false;
    bool m_offline = false;
    QString m_user;
    QString m_status;
    int m_failedAttempts = 0;
    qint64 m_lockUntilMs = 0;
    QTimer *m_lockTimer = nullptr;
};
