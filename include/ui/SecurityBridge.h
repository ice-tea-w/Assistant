#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

class QProcess;
class QTimer;

/// M3 安全模块的 QML↔Python 桥。
///
/// 主程序侧只负责"展示 + 触发"，真正的安全能力在 `tools/file_guard`（Python）。
/// 约定：
///   * 只调用白名单子命令 + `--json`，参数数组交给 QProcess，不拼 shell；
///   * 全部异步、带超时；Python 缺失时降级为只读状态，绝不阻塞启动；
///   * 数据目录固定在 `%LOCALAPPDATA%/<App>/security`，不碰用户真实 `~/.file_guard`。
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
    ~SecurityBridge() override;

    /// 一般不需要调用：会自动从可执行文件向上找仓库根目录（含 tools/file_guard）。
    void setRepoRoot(const QString &repoRoot);

    bool available() const { return m_available; }
    bool busy() const { return m_busy; }
    QString version() const { return m_version; }
    QString summary() const { return m_summary; }
    QVariantList cards() const { return m_cards; }
    QString lastReport() const { return m_lastReport; }

    /// 刷新四张状态卡：doctor / log verify / backup list / vault info。
    Q_INVOKABLE void refresh();
    /// 体检（与 refresh 相同，但会先给 UI 一个"正在检查"的提示）。
    Q_INVOKABLE void runSelfCheck();
    /// 扫描目录（空路径 = Downloads）。
    Q_INVOKABLE void runScan(const QString &path);
    /// 打开端到端验收报告（docs/框架临时验证报告.md）。
    Q_INVOKABLE void openReport();
    /// 打开安全模块数据目录。
    Q_INVOKABLE void openDataDir();

signals:
    void changed();
    void toast(const QString &text);

private:
    struct Probe {
        QString id;
        QStringList args;
        bool json = true;
    };

    void probeEnvironment();
    void enqueue(const QVector<Probe> &probes);
    void startNext();
    void onProbeFinished(int exitCode);
    void handleTimeout();
    void failFast(const QString &message);
    void buildCards();

    static QString locateRepoRoot();
    static QString locatePython(const QString &repoRoot);

    QString m_repoRoot;
    QString m_toolsDir;
    QString m_python;
    QString m_home;
    QString m_version;
    QString m_summary;
    QString m_lastReport;
    QVariantList m_cards;
    bool m_available = false;
    bool m_busy = false;

    QQueue<Probe> m_queue;
    Probe m_current;
    QProcess *m_proc = nullptr;
    QTimer *m_timeout = nullptr;
    bool m_abandon = false;   ///< 超时被 kill 时置位，避免 finished 与 startNext 双重推进
    QMap<QString, QJsonObject> m_results;
    QString m_scanDetail;
    QString m_healthDetail;
};
