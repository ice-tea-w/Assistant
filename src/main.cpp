#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QWindow>
#include <QQmlContext>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>

#include "core/Logger.h"
#include "core/ConfigManager.h"
#include "core/AppIndexer.h"
#include "core/FileSearcher.h"
#include "database/HistoryDatabase.h"
#include "database/TodoDatabase.h"
#include "database/NotesDatabase.h"
#include "database/ClipboardDatabase.h"
#include "ui/TrayManager.h"
#include "core/HotkeyManager.h"
#include "ui/AppBridge.h"
#include "ui/PanelBridge.h"
#include "ui/SyncBridge.h"

// CMake 通过 target_compile_definitions 注入（project VERSION）
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

int main(int argc, char *argv[])
{
    // 走 Fusion 样式：Windows11 原生样式自己画浅色控件底、基本无视 palette，
    // 深色主题下会变成"白底浅字"。Fusion 是纯 QML 自绘，完全跟随窗口 palette。
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    // Required by QStandardPaths::AppConfigLocation (used by ConfigManager)
    // --instance <名字>：同机跑第二个实例时用。配置/数据库/日志目录随应用名分开，
    // 于是两台"设备"能在只有一台电脑的场合互相同步（答辩演示常用）。
    QApplication::setApplicationName("Assistant");
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--instance") == 0) {
            QApplication::setApplicationName(QStringLiteral("Assistant-")
                                             + QString::fromLocal8Bit(argv[i + 1]));
        }
    }
    QApplication::setOrganizationName("AssistantTeam");
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    // ---- config (load before anything that needs it) ----
    ConfigManager config;
    config.load();

    // ---- logging (init after config so we can read logLevel) ----
    QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                     + "/logs";
    Logger::init(logDir.toStdString(), config.logLevel().toStdString());
    Logger::get().info("Assistant v{} starting", APP_VERSION);

    // ---- database ----
    HistoryDatabase historyDb;
    QString dbPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                     + "/history.db";
    historyDb.open(dbPath);

    // ---- Phase 3 databases (Step 2) ----
    const QString dbDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    TodoDatabase todoDb;      todoDb.open(dbDir + "/todo.db");
    NotesDatabase notesDb;    notesDb.open(dbDir + "/notes.db");
    ClipboardDatabase clipDb; clipDb.open(dbDir + "/clipboard.db");

    PanelBridge panel;
    panel.setTodoDatabase(&todoDb);
    panel.setNotesDatabase(&notesDb);
    panel.setClipboardDatabase(&clipDb);
    panel.startClipboardMonitor();

    AppBridge    backend;
    backend.setHistoryDatabase(&historyDb);

    // ---- 设备同步（联网功能）----
    // 桥独立于 PanelBridge，避免把主面板桥变成上帝对象；
    // 具体通道与工作线程都在 SyncBridge 内部按需创建。
    SyncBridge syncBridge;
    syncBridge.setConfigManager(&config);
    syncBridge.init(dbDir + "/sync.db");

    // ---- app indexer (Step 5): register launchable Start Menu apps ----
    for (const AppIndexer::Entry &app : AppIndexer::scan())
        backend.addApp(app.name, app.path);

    // ---- file indexer (Step 6): build filename index for on-the-fly search ----
    FileSearcher::buildIndex();

    TrayManager   tray;

    // 对端投递的文件收进收件箱后弹一条托盘通知
    QObject::connect(&syncBridge, &SyncBridge::filesReceived,
                     &tray, [&tray](int count, const QString &inboxDir) {
        if (count <= 0)
            return;
        tray.notify(QStringLiteral("收到 %1 个文件").arg(count),
                    QStringLiteral("收件箱：") + inboxDir);
    });
    HotkeyManager hotkey;

    // 设置页改热键需要（先注册失败要能回滚，见 PanelBridge::applyHotkey）
    panel.setConfigManager(&config);
    panel.setHotkeyManager(&hotkey);

    QQmlApplicationEngine engine;

    // ---- expose objects to QML ----
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("config",  &config);
    engine.rootContext()->setContextProperty("appVersion", QCoreApplication::applicationVersion());
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

    // ---- main panel window (Phase 3 Step 1) ----
    // 常驻主面板：独立 engine，初始隐藏，由 🏠 按钮 / 托盘菜单触发显示。
    QQmlApplicationEngine panelEngine;
    panelEngine.rootContext()->setContextProperty("config", &config);
    panelEngine.rootContext()->setContextProperty("panel", &panel);
    panelEngine.rootContext()->setContextProperty("sync", &syncBridge);
    panelEngine.rootContext()->setContextProperty("appVersion", QCoreApplication::applicationVersion());
    panelEngine.load(QUrl(QStringLiteral("qrc:/qml/MainPanel.qml")));

    QWindow *panelWindow = nullptr;
    if (!panelEngine.rootObjects().isEmpty())
        panelWindow = qobject_cast<QWindow *>(panelEngine.rootObjects().first());

    // 🏠 按钮 / 托盘「主面板」 → 显示主面板 + 隐藏搜索浮窗
    // （第二个实例抢不到全局热键，托盘菜单是它打开主面板的唯一入口）
    auto showMainPanel = [&]() {
        if (panelWindow) {
            panelWindow->show();
            panelWindow->raise();
            panelWindow->requestActivate();
        }
        backend.hideWindow();
    };
    QObject::connect(&backend, &AppBridge::mainPanelRequested, &app, showMainPanel);
    QObject::connect(&tray, &TrayManager::mainPanelRequested, &app, showMainPanel);

    // 调试开关：带 --show-panel 启动时立即弹主面板（界面检查/截图用）；
    // 附加 --page=N 可直接打开指定页（0 待办 / 1 记事本 / 2 剪贴板 / 3 设备同步 / 4 设置）
    if (qApp->arguments().contains(QStringLiteral("--show-panel"))) {
        QTimer::singleShot(400, &app, [&]() {
            for (const QString &arg : qApp->arguments()) {
                if (!arg.startsWith(QStringLiteral("--page=")))
                    continue;
                bool ok = false;
                const int page = arg.mid(7).toInt(&ok);
                if (ok && page >= 0 && panelWindow)
                    panelWindow->setProperty("selectedIndex", page);
            }
            showMainPanel();
        });
    }

    tray.init();

    return app.exec();
}
