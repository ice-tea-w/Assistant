#include <QApplication>
#include <QQmlApplicationEngine>
#include <QWindow>
#include <QQmlContext>
#include <QStandardPaths>
#include <QDir>

#include "core/Logger.h"
#include "core/ConfigManager.h"
#include "core/AppIndexer.h"
#include "core/FileSearcher.h"
#include "database/HistoryDatabase.h"
#include "ui/TrayManager.h"
#include "core/HotkeyManager.h"
#include "ui/AppBridge.h"

int main(int argc, char *argv[])
{
    // Required by QStandardPaths::AppConfigLocation (used by ConfigManager)
    QApplication::setApplicationName("Assistant");
    QApplication::setOrganizationName("AssistantTeam");

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    // ---- config (load before anything that needs it) ----
    ConfigManager config;
    config.load();

    // ---- logging (init after config so we can read logLevel) ----
    QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                     + "/logs";
    Logger::init(logDir.toStdString(), config.logLevel().toStdString());
    Logger::get().info("Assistant v{} starting", "0.1.0");

    // ---- database ----
    HistoryDatabase historyDb;
    QString dbPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                     + "/history.db";
    historyDb.open(dbPath);

    AppBridge    backend;
    backend.setHistoryDatabase(&historyDb);

    // ---- app indexer (Step 5): register launchable Start Menu apps ----
    for (const AppIndexer::Entry &app : AppIndexer::scan())
        backend.addApp(app.name, app.path);

    // ---- file indexer (Step 6): build filename index for on-the-fly search ----
    FileSearcher::buildIndex();

    TrayManager   tray;
    HotkeyManager hotkey;
    QQmlApplicationEngine engine;

    // ---- expose objects to QML ----
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("config",  &config);
    engine.load(QUrl(QStringLiteral("qrc:/qml/MainWindow.qml")));

    if (engine.rootObjects().isEmpty())
        return -1;

    QObject *rootObject = engine.rootObjects().first();
    QWindow *window = qobject_cast<QWindow *>(rootObject);

    // ---- C++ signal/slot wiring ----

    // Tray "Show/Hide" → toggle window via bridge
    QObject::connect(&tray, &TrayManager::toggleWindowRequested,
                     &backend, &AppBridge::toggleWindow);

    // Tray "Quit" → exit application
    QObject::connect(&tray, &TrayManager::quitRequested,
                     qApp, &QApplication::quit);

    // Global hotkey → toggle window via bridge
    QObject::connect(&hotkey, &HotkeyManager::hotkeyPressed,
                     &backend, &AppBridge::toggleWindow);

    // Window hidden → persist position
    QObject::connect(&backend, &AppBridge::visibleChanged, [&]() {
        if (!backend.isVisible() && window) {
            config.saveWindowPosition(window->position().x(),
                                      window->position().y());
        }
    });

    // Register global hotkey (from config, not hardcoded)
    void *hwnd = nullptr;
    if (window)
        hwnd = reinterpret_cast<void *>(window->winId());

    if (hwnd) {
        hotkey.registerHotkey(config.hotkeyModifiers(),
                              config.hotkeyKey(),
                              hwnd);
        app.installNativeEventFilter(&hotkey);
    }

    // Restore window position (if previously saved)
    if (config.windowX() >= 0 && config.windowY() >= 0) {
        window->setPosition(config.windowX(), config.windowY());
    }

    tray.init();

    return app.exec();
}
