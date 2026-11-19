pragma Singleton
import QtQuick

// 全局设计令牌（Design Tokens）
//
// 2026-11-19 UI 改版：原来只有 7 个颜色 + 3 个字号，页面各自再派生一份。
// 现在收敛为「一套令牌」：
//   - 颜色：深色下越靠前越亮，浅色下越靠前越白；
//   - 间距：4pt 基数；
//   - 字号：Display / Title / Subtitle / Body / Label / Caption / Micro；
//   - 动效：fast / base / slow 三档。
//
// 旧字段（bgColor/panelColor/...）保留为别名，页面可以逐页迁移，不用一次性全改。
QtObject {
    id: root

    readonly property bool isDark: (typeof config !== "undefined") && config.theme === "dark"

    // ================= 颜色 =================
    // 背景 / 表面
    readonly property color bg:          isDark ? "#171A1F" : "#F7F8FB"
    readonly property color navBg:       isDark ? "#1B1F25" : "#F1F4F9"
    readonly property color surface:     isDark ? "#1D2127" : "#FFFFFF"
    readonly property color surface2:    isDark ? "#242A32" : "#EEF1F6"
    readonly property color surface3:    isDark ? "#2C333D" : "#E4E9F1"

    // 分隔线
    readonly property color line:        isDark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(20 / 255, 29 / 255, 45 / 255, 0.08)
    readonly property color lineStrong:  isDark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(20 / 255, 29 / 255, 45 / 255, 0.14)

    // 文字
    readonly property color text:        isDark ? "#E9ECF1" : "#151A22"
    readonly property color text2:       isDark ? "#AAB3C0" : "#525D6E"
    readonly property color text3:       isDark ? "#808A99" : "#6B7585"

    // 强调色（只用于：主按钮 / 当前选中 / 关键数字）
    readonly property color accent:        isDark ? "#6E8DFF" : "#2F5FE0"
    readonly property color accentHover:   isDark ? "#7E9BFF" : "#2554D6"
    readonly property color accentPressed: isDark ? "#5F7FE8" : "#1E46B8"
    readonly property color accent2:       isDark ? "#9B7BFF" : "#6F49E8"
    readonly property color accentSoft:    isDark ? Qt.rgba(110 / 255, 141 / 255, 1, 0.14)
                                                  : Qt.rgba(47 / 255, 95 / 255, 224 / 255, 0.10)
    readonly property color accentBorder:  isDark ? Qt.rgba(110 / 255, 141 / 255, 1, 0.38)
                                                  : Qt.rgba(47 / 255, 95 / 255, 224 / 255, 0.30)
    readonly property color textOnAccent: isDark ? "#101216" : "#FFFFFF"
    readonly property color textOnDanger: "#FFFFFF"

    // 语义色
    readonly property color success: isDark ? "#45C48C" : "#159A63"
    readonly property color warning: isDark ? "#EEB154" : "#B57311"
    readonly property color danger:  isDark ? "#EF6B6B" : "#D14848"
    readonly property color dangerSoft: isDark ? Qt.rgba(239 / 255, 107 / 255, 107 / 255, 0.14)
                                               : Qt.rgba(209 / 255, 72 / 255, 72 / 255, 0.09)

    // 文件类型色（搜索结果 / 下载列表）
    readonly property color tileFile: isDark ? "#93AAFF" : "#2F5FE0"
    readonly property color tileApp:  isDark ? "#63D3A4" : "#12855A"
    readonly property color tileDoc:  isDark ? "#F0BD72" : "#A2650D"
    readonly property color tileCalc: isDark ? "#B49BFF" : "#6B4CE0"
    readonly property color tileFileBg: isDark ? Qt.rgba(110 / 255, 141 / 255, 1, 0.16)
                                               : Qt.rgba(47 / 255, 95 / 255, 224 / 255, 0.10)
    readonly property color tileAppBg:  isDark ? Qt.rgba(69 / 255, 196 / 255, 140 / 255, 0.15)
                                               : Qt.rgba(21 / 255, 154 / 255, 99 / 255, 0.10)
    readonly property color tileDocBg:  isDark ? Qt.rgba(238 / 255, 177 / 255, 84 / 255, 0.15)
                                               : Qt.rgba(181 / 255, 115 / 255, 17 / 255, 0.11)
    readonly property color tileCalcBg: isDark ? Qt.rgba(155 / 255, 123 / 255, 1, 0.16)
                                               : Qt.rgba(122 / 255, 90 / 255, 245 / 255, 0.11)

    // ================= 尺寸 =================
    readonly property int spacingXs: 4
    readonly property int spacingS:  8
    readonly property int spacingM:  12
    readonly property int spacingL:  16
    readonly property int spacingXl: 20
    readonly property int spacing2xl: 24

    readonly property int radiusSmall:   6
    readonly property int radiusControl: 8
    readonly property int radiusInput:   10
    readonly property int radiusCard:    12
    readonly property int radiusWindow:  16
    readonly property int radius:        12   // 旧名：卡片圆角

    readonly property int pagePadding: 20
    readonly property int cardPadding: 14
    readonly property int controlHeight: 34
    readonly property int titlebarHeight: 44
    readonly property int navWidth: 158
    readonly property int rowHeight: 44

    // ================= 字体 =================
    readonly property string fontFamily: "Segoe UI Variable Text"
    readonly property int fontDisplay: 22
    readonly property int fontTitle: 19
    readonly property int fontSubtitle: 15
    readonly property int fontBody: 13
    readonly property int fontLabel: 12
    readonly property int fontCaption: 11
    readonly property int fontMicro: 10

    // 旧字号别名
    readonly property int titleSize: fontTitle
    readonly property int bodySize: fontBody
    readonly property int captionSize: fontCaption

    // ================= 动效 =================
    readonly property int motionFast: 90
    readonly property int motionBase: 140
    readonly property int motionSlow: 180

    // ================= 旧颜色别名（逐页迁移期间兼容） =================
    readonly property color bgColor:         bg
    readonly property color panelColor:      surface
    readonly property color textColor:       text
    readonly property color mutedColor:      text3
    readonly property color accentColor:     accent
    readonly property color borderColor:     line
    readonly property color activeBorder:    accentBorder
    readonly property color controlColor:    surface2
    readonly property color statusTextColor: text3
    readonly property color accentTextColor: textOnAccent
    readonly property color dangerColor:     danger

    /// 给任意颜色加透明度（QML 里没有内置的 alpha 函数）
    function alpha(c, a) {
        return Qt.rgba(c.r, c.g, c.b, a)
    }
}
