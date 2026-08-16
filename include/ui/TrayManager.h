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

signals:
    void toggleWindowRequested();
    void quitRequested();

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu           *m_trayMenu = nullptr;
    QAction         *m_showAction = nullptr;
    QAction         *m_quitAction = nullptr;
};
