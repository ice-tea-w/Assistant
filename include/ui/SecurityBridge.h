#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

/// 安全中心桥（**C++ 直连版**）
///
/// 直接调用编译进主程序的 C++ 安全模块（tools/file_guard_cpp）：
/// 审计链校验、备份库、保险箱状态、敏感扫描；不再依赖 Python / QProcess。
///
/// 数据目录：%LOCALAPPDATA%/AssistantTeam/Assistant/security
/// 其中 app.key 是 DPAPI 不涉及的应用密钥（随机 32B，ACL 收紧），
/// 用于审计/备份的本地加密索引（不是用户口令，避免启动时要求输入密码）。
class SecurityBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QVariantList cards READ cards NOTIFY changed)
    Q_PROPERTY(QString lastReport READ lastReport NOTIFY changed)

public:
    explicit SecurityBridge(QObject *parent = nullptr);

    bool available() const { return m_available; }
    bool busy() const { return m_busy; }
    QString version() const { return m_version; }
    QString summary() const { return m_summary; }
    QVariantList cards() const { return m_cards; }
    QString lastReport() const { return m_lastReport; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void runSelfCheck();
    Q_INVOKABLE void runScan(const QString &path);
    Q_INVOKABLE void openReport();
    Q_INVOKABLE void openDataDir();

signals:
    void changed();
    void toast(const QString &text);

private:
    void buildCards();
    void writeReport() const;
    QString dataDir() const;

    bool m_available = true;
    bool m_busy = false;
    QString m_version = QStringLiteral("1.0.0 (C++)");
    QString m_summary = QStringLiteral("正在检查…");
    QVariantList m_cards;
    QString m_lastReport;

    // 最近一次检查结果
    bool m_auditOk = false;
    quint64 m_auditRecords = 0;
    QString m_auditHead;
    int m_backupCount = 0;
    quint64 m_backupBytes = 0;
    bool m_backupOk = false;
    bool m_vaultExists = false;
    QString m_scanDetail = QStringLiteral("未扫描");
};
