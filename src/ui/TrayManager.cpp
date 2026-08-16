#include "ui/TrayManager.h"

#include <QApplication>
#include <QPainter>
#include <QPixmap>

TrayManager::TrayManager(QObject *parent)
    : QObject(parent)
{
}

TrayManager::~TrayManager()
{
}

void TrayManager::init()
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(QColor("#4A90D9"));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(4, 4, 24, 24);
    }

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(QIcon(pixmap));
    m_trayIcon->setToolTip("Assistant");

    m_trayMenu = new QMenu();
    m_showAction = m_trayMenu->addAction("Show / Hide");
    m_trayMenu->addSeparator();
    m_quitAction = m_trayMenu->addAction("Quit");

    m_trayIcon->setContextMenu(m_trayMenu);

    connect(m_showAction, &QAction::triggered, this, &TrayManager::toggleWindowRequested);
    connect(m_quitAction, &QAction::triggered, this, &TrayManager::quitRequested);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &TrayManager::onTrayActivated);

    m_trayIcon->show();
}

void TrayManager::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
        emit toggleWindowRequested();
    }
}
