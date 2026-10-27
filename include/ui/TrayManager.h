#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>

class TrayManager : public QObject
{
    Q_OBJECT

public:
    explicit TrayManager(QObject *parent = nullptr);
    ~TrayManager() override;

    void init();

    /// 托盘气泡通知（收到投递文件等）
    void notify(const QString &title, const QString &body);

signals:
    void toggleWindowRequested();
    void mainPanelRequested();
    void quitRequested();

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu           *m_trayMenu = nullptr;
    QAction         *m_showAction = nullptr;
    QAction         *m_panelAction = nullptr;
    QAction         *m_quitAction = nullptr;
};
