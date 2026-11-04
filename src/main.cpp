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
#include "core/FileIndex.h"
#include "database/HistoryDatabase.h"
#include "database/TodoDatabase.h"
#include "database/NotesDatabase.h"
#include "database/ClipboardDatabase.h"
#include "ui/TrayManager.h"
#include "core/HotkeyManager.h"
#include "ui/AppBridge.h"
#include "ui/PanelBridge.h"
#include "ui/PluginBridge.h"
#include "core/plugin/PluginCatalog.h"
#include "core/plugin/PluginInstaller.h"
#include "core/plugin/PluginLoader.h"
#include "core/plugin/PluginManager.h"
#include "core/plugin/PluginRegistry.h"
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

    // ---- plugin subsystem (插件市场 M0：本地插件管理) ----
    // 装配点是唯一的：四个能力模块彼此不认识，只有 PluginManager 持有它们。
    PluginRegistry pluginRegistry;
    {
        QString warning;
        if (!pluginRegistry.load(&warning)) {
            // Not fatal: the app runs without plugins, the market page just stays empty.
            Logger::get().error("Plugins: cannot prepare {}",
                                pluginRegistry.rootDir().toStdString());
        } else if (!warning.isEmpty()) {
            Logger::get().warn("Plugins: {}", warning.toStdString());
        }
    }

    PluginLoader    pluginLoader;
    PluginCatalog   pluginCatalog(&pluginRegistry);
    PluginInstaller pluginInstaller(&pluginRegistry);
    PluginManager   pluginManager(&pluginRegistry, &pluginLoader);
    PluginBridge    pluginBridge(&pluginManager);

    pluginManager.setInstaller(&pluginInstaller);
    pluginManager.setCatalog(&pluginCatalog);

    // 市场索引地址。默认留空 = 只用内置 seed：离线也能看到内容，答辩现场不怕断网。
    // 想接真服务器：设环境变量 ASSISTANT_MARKET_URL 指向一个 index.json。
    pluginCatalog.setMarketUrl(qEnvironmentVariable("ASSISTANT_MARKET_URL"));

    // ---- main panel window (Phase 3 Step 1) ----
    // 常驻主面板：独立 engine，初始隐藏，由 🏠 按钮 / 托盘菜单触发显示。
    QQmlApplicationEngine panelEngine;
    panelEngine.rootContext()->setContextProperty("config", &config);
    panelEngine.rootContext()->setContextProperty("panel", &panel);
    panelEngine.rootContext()->setContextProperty("sync", &syncBridge);
    panelEngine.rootContext()->setContextProperty("appVersion", QCoreApplication::applicationVersion());

    // Must be registered BEFORE the QML loads: MainPanel binds `pluginApi` to
    // `plugins` at creation time and falls back to mock data when it is absent.
    panelEngine.rootContext()->setContextProperty("plugins", &pluginBridge);
    pluginManager.setEngine(&panelEngine);

    panelEngine.load(QUrl(QStringLiteral("qrc:/qml/MainPanel.qml")));

    // Plugins load after the engine exists but before the window is shown, so the
    // navigation list is complete on first paint instead of popping in later.
    // A failing plugin is recorded as its own problem and never blocks the rest.
    pluginManager.setDark(config.theme() == QLatin1String("dark"));
    pluginManager.loadEnabledAtStartup();

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

    const int exitCode = app.exec();

    // Plugin objects must be destroyed while the engine is still alive.
    pluginManager.unloadAll();

    return exitCode;
}
