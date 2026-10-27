import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Phase 3 主面板 —— 常驻窗口（区别于搜索浮窗）：
// 不置顶、非 Tool、失焦不隐藏、可最小化。
// 左侧功能栏：待办 / 记事本 / 剪贴板（从上到下）+ 设置（固定底部）。
// 右侧 StackLayout 切换对应页面。
ApplicationWindow {
    id: panelWindow
    width: 760
    height: 520
    title: "Assistant"
    color: "transparent"
    visible: false   // 由 🏠 按钮 / 托盘菜单触发显示

    // ---- Theme colors：统一来自 Style 单例（同目录 qmldir 的 singleton）----
    readonly property bool isDark: Style.isDark
    readonly property color bgColor:     Style.bgColor
    readonly property color panelColor:  Style.panelColor
    readonly property color textColor:   Style.textColor
    readonly property color mutedColor:  Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor

    property int selectedIndex: 0

    // 左侧导航前三项（从上到下）；设置项单独固定在底部。
    property var navItems: [
        { "icon": "✓",  "label": "待办事项" },
        { "icon": "📝", "label": "记事本" },
        { "icon": "📋", "label": "剪贴板" },
        { "icon": "🔄", "label": "设备同步" }
    ]
    // 设置页在 StackLayout 里的下标（= 导航项数量）
    readonly property int settingsIndex: navItems.length

    // 点 X 只隐藏，不销毁（主面板常驻，下次 🏠 重新 show）
    onClosing: function(close) {
        close.accepted = false
        panelWindow.hide()
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: bgColor
        border.color: borderColor
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            // ---- 左侧导航栏 ----
            Rectangle {
                Layout.preferredWidth: 150
                Layout.fillHeight: true
                radius: 8
                color: panelColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    // 前三项功能入口（从上到下）
                    ListView {
                        id: navList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: panelWindow.navItems
                        currentIndex: panelWindow.selectedIndex < panelWindow.navItems.length
                                      ? panelWindow.selectedIndex : -1
                        spacing: 4

                        delegate: Rectangle {
                            width: ListView.view.width
                            height: 40
                            radius: 6
                            color: index === navList.currentIndex
                                   ? accentColor : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8

                                Text {
                                    text: modelData.icon
                                    font.pixelSize: 14
                                }
                                Text {
                                    text: modelData.label
                                    color: index === navList.currentIndex
                                           ? "white" : textColor
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    navList.currentIndex = index
                                    panelWindow.selectedIndex = index
                                }
                            }
                        }
                    }

                    // 分隔线
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: borderColor
                    }

                    // ---- 设置（固定底部）----
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        radius: 6
                        color: panelWindow.selectedIndex === panelWindow.settingsIndex
                               ? accentColor : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Text { text: "⚙"; font.pixelSize: 14 }
                            Text {
                                text: "设置"
                                color: panelWindow.selectedIndex === panelWindow.settingsIndex
                                       ? "white" : textColor
                                font.pixelSize: 14
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                navList.currentIndex = -1
                                panelWindow.selectedIndex = panelWindow.settingsIndex
                            }
                        }
                    }
                }
            }

            // ---- 右侧功能区 ----
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: panelWindow.selectedIndex

                TodoPage {}
                NotesPage {}
                ClipboardPage {}
                SyncPage {}
                SettingsPage {}
            }
        }
    }
}
