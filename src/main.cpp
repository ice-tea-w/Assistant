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
#include "core/FileIndex.h"
#include "database/HistoryDatabase.h"
#include "database/TodoDatabase.h"
#include "database/NotesDatabase.h"
#include "database/ClipboardDatabase.h"
#include "ui/TrayManager.h"
#include "core/HotkeyManager.h"
#include "ui/AppBridge.h"
#include "ui/PanelBridge.h"

// CMake 通过 target_compile_definitions 注入（project VERSION）
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

int main(int argc, char *argv[])
{
    // Required by QStandardPaths::AppConfigLocation (used by ConfigManager)
    QApplication::setApplicationName("Assistant");
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

    // ---- app indexer (Step 5): register launchable Start Menu apps ----
    for (const AppIndexer::Entry &app : AppIndexer::scan())
        backend.addApp(app.name, app.path);

    // ---- file indexer (Step 6 / A2 + A3) ----
    // v2      : FileIndex 门面 + 后台扫描（FileScanner + FileIndexDatabase 差量）。
    //           startAsync() **立即返回**，窗口与热键先可用，索引就绪后 AppBridge
    //           自动重跑最近一次查询（修 P0-4）。
    // legacy  : 旧 FileSearcher 同步建索引，行为与 A1 **完全一致**，是 v2 的退路。
    // 开关落在 config.json 的 fileIndex.engine，一行配置就能切回去。
    FileIndex fileIndex;
    const bool useFileIndexV2 = (config.fileIndexEngine() == QStringLiteral("v2"));
    if (useFileIndexV2) {
        // file_index.db 是**派生数据**（可随时重扫重建），和 history.db 等平级存放
        fileIndex.setDatabasePath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                                  + QStringLiteral("/file_index.db"));
        fileIndex.setEntryLimit(config.fileIndexHardLimitEntries());
        backend.setFileIndex(&fileIndex);
        fileIndex.startAsync();          // 放在 engine.load() 之前，但不再阻塞
    } else {
        FileSearcher::buildIndex();
    }
    Logger::get().info("FileIndex: engine = {}（硬上限 {} 条）",
                       useFileIndexV2 ? "v2" : "legacy",
                       config.fileIndexHardLimitEntries());

    TrayManager   tray;
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

    // A2 验收①的观测点：这一行证明「窗口已经起来了」时索引处于什么状态。
    // legacy 会先打印 FileSearcher: indexed … 再打印这一行（同步建索引，UI 被挡住）；
    // v2 反过来 —— 这一行在前，FileIndex: published snapshot 在后。
    if (useFileIndexV2) {
        const FileIndex::IndexStats st = fileIndex.stats();
        Logger::get().info("UI loaded — fileIndex v2 ready={} scanning={} entries={}",
                           st.ready, st.scanning, st.entries);
    } else {
        Logger::get().info("UI loaded — fileIndex legacy ({} files indexed synchronously)",
                           FileSearcher::size());
    }

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
    panelEngine.rootContext()->setContextProperty("appVersion", QCoreApplication::applicationVersion());
    panelEngine.load(QUrl(QStringLiteral("qrc:/qml/MainPanel.qml")));

    QWindow *panelWindow = nullptr;
    if (!panelEngine.rootObjects().isEmpty())
        panelWindow = qobject_cast<QWindow *>(panelEngine.rootObjects().first());

    // 🏠 按钮 → 显示主面板 + 隐藏搜索浮窗
    QObject::connect(&backend, &AppBridge::mainPanelRequested, [&]() {
        if (panelWindow) {
            panelWindow->show();
            panelWindow->raise();
            panelWindow->requestActivate();
        }
        backend.hideWindow();
    });

    tray.init();

    return app.exec();
}
