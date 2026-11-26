#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

/// 安全中心桥（C++ 直连）
///
/// 直接调用编译进主程序的 C++ 安全模块（tools/file_guard_cpp）：
/// 审计链、备份库、保险箱状态、敏感扫描；无 Python / 无子进程。
class SecurityBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QVariantList cards READ cards NOTIFY changed)
    Q_PROPERTY(QString lastReport READ lastReport NOTIFY changed)

    /// 体检过程：每完成一项追加一条 {title, state(ok/error), detail, time}
    Q_PROPERTY(QVariantList healthSteps READ healthSteps NOTIFY healthChanged)
    Q_PROPERTY(bool healthRunning READ healthRunning NOTIFY healthChanged)

public:
    explicit SecurityBridge(QObject *parent = nullptr);

    bool available() const { return m_available; }
    bool busy() const { return m_busy; }
    QString version() const { return m_version; }
    QString summary() const { return m_summary; }
    QVariantList cards() const { return m_cards; }
    QString lastReport() const { return m_lastReport; }
    QVariantList healthSteps() const { return m_healthSteps; }
    bool healthRunning() const { return m_healthRunning; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void runSelfCheck();
    Q_INVOKABLE void runScan(const QString &path);

    /// 清空审计链并写入一条“手动重置”记录（不可恢复）
    Q_INVOKABLE void resetAudit();
    Q_INVOKABLE void openReport();
    Q_INVOKABLE void openDataDir();

    /// 点击卡片：打开对应目录/报告（audit/backup/vault/scan）
    Q_INVOKABLE void openCard(const QString &key);

signals:
    void changed();
    void healthChanged();
    void toast(const QString &text);

private:
    void buildCards();
    void writeReport() const;
    QString dataDir() const;

    bool checkDataDir(QString *detail);
    bool checkAppKey(QString *detail);
    bool checkAudit(QString *detail);
    bool checkBackup(QString *detail);
    void checkVault(QString *detail);

    void startHealthCheck();
    void runNextStep();
    void appendStep(const QString &title, const QString &state, const QString &detail);

    bool m_available = true;
    bool m_busy = false;
    bool m_healthRunning = false;
    int m_stepIndex = 0;

    QString m_version = QStringLiteral("1.0.0");
    QString m_summary = QStringLiteral("正在检查…");
    QVariantList m_cards;
    QVariantList m_healthSteps;
    QString m_lastReport;

    bool m_auditOk = false;
    quint64 m_auditRecords = 0;
    QString m_auditHead;
    int m_backupCount = 0;
    quint64 m_backupBytes = 0;
    bool m_backupOk = false;
    bool m_vaultExists = false;
    QString m_scanDetail = QStringLiteral("未扫描");
};
