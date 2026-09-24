pragma Singleton
import QtQuick

// 全局主题单例（Phase 3 收尾）
//
// 以前每个 QML 页面各自从 config.theme 派生一份颜色（MainWindow / MainPanel /
// TodoPage / NotesPage / ClipboardPage / SettingsPage 共 6 处），改一个色值要改 6 遍。
// 现在统一到这里，页面通过同目录的 qmldir 隐式导入即可直接使用 `Style.xxx`。
//
// 注：`config` 是 C++ 注入的 context property，两个 engine（搜索浮窗 / 主面板）
// 都设置了它，所以本单例在哪边实例化都能取到。
QtObject {
    readonly property bool isDark: config.theme === "dark"

    // ---- 颜色 ----
    readonly property color bgColor:      isDark ? "#2c2c2c" : "#f0f0f0"
    readonly property color panelColor:   isDark ? "#1e1e1e" : "#ffffff"
    readonly property color textColor:    isDark ? "#e0e0e0" : "#1a1a1a"
    readonly property color mutedColor:   isDark ? "#666666" : "#999999"
    readonly property color accentColor:  isDark ? "#3a6ea5" : "#4a90d9"
    readonly property color borderColor:  isDark ? "#3d3d3d" : "#d0d0d0"
    readonly property color activeBorder: isDark ? "#5a8ec9" : "#4a90d9"

    // 状态栏文字（搜索浮窗底部"3 result(s)"那行）
    readonly property color statusTextColor: isDark ? "#888888" : "#777777"

    // 选中/悬停时的前景色（铺在 accentColor 背景上）
    // 注意：**不能**叫 `onAccentColor` —— QML 把 `on` + 大写开头的标识符当作
    // 信号处理器（signal handler）解析，会直接报
    // "Cannot assign a value to a signal (expecting a script to be run)"。
    readonly property color accentTextColor: "#ffffff"

    // 错误提示色（热键注册失败等）
    readonly property color dangerColor: isDark ? "#e0716b" : "#d0453c"

    // ---- 尺寸 ----
    readonly property int radius:        8
    readonly property int radiusSmall:   6
    readonly property int titleSize:     18
    readonly property int bodySize:      13
    readonly property int captionSize:   11
}
