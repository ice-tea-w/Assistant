import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 设置页（Phase 3 Step 6 + 收尾）
// 主题 / 日志级别 / 全局热键（可自定义）/ 开机启动
// 主题色统一走 Style 单例（同目录 qmldir 声明的 singleton）。
Rectangle {
    id: settingsPage
    color: "transparent"

    readonly property bool isDark:      Style.isDark
    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor

    // ---- 热键捕获状态 ----
    property bool capturing: false
    property string hotkeyStatus: ""
    property bool   hotkeyStatusOk: false

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 20

        Text {
            text: "设置"
            color: textColor
            font.pixelSize: Style.titleSize
            font.bold: true
        }

        // ---- 主题 ----
        ColumnLayout {
            spacing: 8

            Text {
                text: "主题"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                Rectangle {
                    width: 70
                    height: 32
                    radius: Style.radiusSmall
                    color: config.theme === "light" ? accentColor : panelColor
                    border.color: borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "浅色"
                        color: config.theme === "light" ? Style.accentTextColor : textColor
                        font.pixelSize: Style.bodySize
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: config.setTheme("light")
                    }
                }

                Rectangle {
                    width: 70
                    height: 32
                    radius: Style.radiusSmall
                    color: config.theme === "dark" ? accentColor : panelColor
                    border.color: borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "深色"
                        color: config.theme === "dark" ? Style.accentTextColor : textColor
                        font.pixelSize: Style.bodySize
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: config.setTheme("dark")
                    }
                }
            }
        }

        // ---- 日志级别 ----
        ColumnLayout {
            spacing: 8

            Text {
                text: "日志级别"
                color: mutedColor
                font.pixelSize: 12
            }

            ComboBox {
                id: logLevelBox
                model: ["trace", "debug", "info", "warn"]
                Layout.preferredWidth: 140

                Component.onCompleted: {
                    for (let i = 0; i < model.length; i++) {
                        if (model[i] === config.logLevel) {
                            currentIndex = i
                            break
                        }
                    }
                }

                onActivated: config.setLogLevel(currentText)
            }
        }

        // ---- 全局热键（可自定义，Phase 3 收尾）----
        ColumnLayout {
            spacing: 8

            Text {
                text: "全局热键"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                // 点击进入捕获态，接着按下的组合键即为新热键
                Rectangle {
                    width: 190
                    height: 32
                    radius: Style.radiusSmall
                    color: settingsPage.capturing ? accentColor : panelColor
                    border.color: settingsPage.capturing ? accentColor : borderColor
                    border.width: settingsPage.capturing ? 2 : 1

                    Text {
                        anchors.centerIn: parent
                        text: settingsPage.capturing
                              ? "请按下组合键…（Esc 取消）"
                              : config.hotkeyModifiers + " + " + config.hotkeyKey
                        color: settingsPage.capturing ? Style.accentTextColor : textColor
                        font.pixelSize: Style.bodySize
                        font.bold: !settingsPage.capturing
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            settingsPage.capturing = true
                            settingsPage.hotkeyStatus = ""
                            keyCapture.forceActiveFocus()
                        }
                    }
                }

                Rectangle {
                    visible: settingsPage.capturing
                    width: 56
                    height: 32
                    radius: Style.radiusSmall
                    color: borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "取消"
                        color: textColor
                        font.pixelSize: 12
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: settingsPage.capturing = false
                    }
                }
            }

            Text {
                visible: settingsPage.hotkeyStatus.length > 0
                text: settingsPage.hotkeyStatus
                color: settingsPage.hotkeyStatusOk ? mutedColor : Style.dangerColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.maximumWidth: 460
            }

            Text {
                text: "至少包含一个修饰键（Ctrl / Alt / Shift / Win）+ 字母、数字、F1–F24 或空格"
                color: mutedColor
                font.pixelSize: Style.captionSize
            }
        }

        // ---- 开机启动（Phase 3 收尾）----
        ColumnLayout {
            spacing: 8

            Text {
                text: "开机启动"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                Switch {
                    id: autoStartSwitch
                    checked: config.autoStart
                    onToggled: config.setAutoStart(checked)
                }

                Text {
                    text: config.autoStart ? "已随系统启动（写入用户注册表 Run 键）"
                                           : "不随系统启动"
                    color: mutedColor
                    font.pixelSize: 12
                }
            }
        }

        // ---- 关于 ----
        Text {
            text: "Assistant v" + appVersion + " · Phase 3"
            color: mutedColor
            font.pixelSize: Style.captionSize
            Layout.fillWidth: true
        }
    }

    // ---- 隐藏的按键捕获器（放在布局之外，避免影响 ColumnLayout）----
    Item {
        id: keyCapture
        width: 0
        height: 0
        focus: settingsPage.capturing

        // 把 Qt 的按键值翻译成 HotkeyManager::parseKey 认识的名字
        function keyNameOf(k) {
            if (k >= Qt.Key_A && k <= Qt.Key_Z) return String.fromCharCode(k)
            if (k >= Qt.Key_0 && k <= Qt.Key_9) return String.fromCharCode(k)
            if (k >= Qt.Key_F1 && k <= Qt.Key_F24) return "F" + (k - Qt.Key_F1 + 1)
            if (k === Qt.Key_Space)  return "Space"
            if (k === Qt.Key_Escape) return "Escape"
            if (k === Qt.Key_Tab)    return "Tab"
            if (k === Qt.Key_Return || k === Qt.Key_Enter) return "Return"
            return ""
        }

        Keys.onPressed: function(event) {
            if (!settingsPage.capturing) return

            const mods = []
            if (event.modifiers & Qt.ControlModifier) mods.push("Ctrl")
            if (event.modifiers & Qt.AltModifier)     mods.push("Alt")
            if (event.modifiers & Qt.ShiftModifier)   mods.push("Shift")
            if (event.modifiers & Qt.MetaModifier)    mods.push("Win")

            // 单独按 Esc：取消捕获
            if (event.key === Qt.Key_Escape && mods.length === 0) {
                event.accepted = true
                settingsPage.capturing = false
                return
            }

            // 只按下修饰键本身：等待主键，不当作组合键
            if (event.key === Qt.Key_Control || event.key === Qt.Key_Alt ||
                event.key === Qt.Key_Shift   || event.key === Qt.Key_Meta) {
                event.accepted = true
                return
            }

            event.accepted = true

            if (mods.length === 0) {
                settingsPage.hotkeyStatus = "请至少包含一个修饰键（Ctrl / Alt / Shift / Win）"
                settingsPage.hotkeyStatusOk = false
                return
            }

            const keyName = keyNameOf(event.key)
            if (keyName === "") {
                settingsPage.hotkeyStatus = "不支持的按键：请用字母 / 数字 / F1–F24 / 空格 / Tab / 回车"
                settingsPage.hotkeyStatusOk = false
                return
            }

            const combo = mods.join("+")
            if (panel.applyHotkey(combo, keyName)) {
                settingsPage.hotkeyStatus = "已更新为 " + mods.join(" + ") + " + " + keyName
                settingsPage.hotkeyStatusOk = true
                settingsPage.capturing = false
            } else {
                settingsPage.hotkeyStatus = "该组合键被其他程序占用，注册失败（已保留原热键 " +
                                            config.hotkeyModifiers + " + " + config.hotkeyKey + "）"
                settingsPage.hotkeyStatusOk = false
            }
        }
    }
}
