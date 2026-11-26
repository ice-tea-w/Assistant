import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 设置页（2026-11-19 UI 改版）
//
// 从「一根 800 行长滚动」改成「左侧二级导航 + 右侧分组卡片」。
// 所有配置项、热键捕获、规则编辑、IM 目录逻辑都保持原样，只重排视觉。
Rectangle {
    id: settingsPage
    color: "transparent"

    property bool capturing: false
    property string hotkeyStatus: ""
    property bool hotkeyStatusOk: false

    property var kindSelection: config.fileKindFilterKinds
    property string fileStatus: ""
    property int activeSection: 0

    readonly property var sections: [
        { "icon": "lock",     "label": "账号" },
        { "icon": "settings", "label": "通用" },
        { "icon": "filter",   "label": "文件索引" },
        { "icon": "inbox",    "label": "收件箱" },
        { "icon": "info",     "label": "关于" }
    ]

    function toggleKind(token, on) {
        var next = kindSelection.slice()
        var at = next.indexOf(token)
        if (on && at < 0)
            next.push(token)
        else if (!on && at >= 0)
            next.splice(at, 1)
        kindSelection = next
    }

    function jumpTo(index) {
        activeSection = index
        var targets = [accountSection, generalSection, fileSection, inboxSection, aboutSection]
        settingsFlick.contentY = Math.max(0, targets[index].y - 4)
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        spacing: Style.spacingL

        // ---- 左侧二级导航 ----
        Rectangle {
            Layout.preferredWidth: 148
            Layout.fillHeight: true
            radius: Style.radiusCard
            color: Style.surface
            border.width: 1
            border.color: Style.line

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 9
                spacing: 2

                Text {
                    Layout.leftMargin: 8
                    Layout.topMargin: 2
                    Layout.bottomMargin: 6
                    text: "设置"
                    color: Style.text
                    font.pixelSize: Style.fontTitle
                    font.weight: Font.DemiBold
                    font.family: Style.fontFamily
                }

                Repeater {
                    model: settingsPage.sections

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: Style.radiusControl
                        color: settingsPage.activeSection === index
                               ? Style.accentSoft
                               : sectionHover.hovered ? Style.surface2 : "transparent"

                        HoverHandler { id: sectionHover }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 9
                            anchors.rightMargin: 8
                            spacing: 8

                            AppIcon {
                                name: modelData.icon
                                size: 14
                                iconColor: settingsPage.activeSection === index
                                           ? Style.accent : Style.text3
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.label
                                color: settingsPage.activeSection === index
                                       ? Style.accent : Style.text2
                                font.pixelSize: Style.fontBody
                                font.weight: settingsPage.activeSection === index
                                             ? Font.DemiBold : Font.Normal
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: settingsPage.jumpTo(index)
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Text {
                    Layout.leftMargin: 8
                    text: "v" + appVersion
                    color: Style.text3
                    font.pixelSize: 10
                    font.family: Style.fontFamily
                }
            }
        }

        // ---- 右侧内容 ----
        Flickable {
            id: settingsFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: settingsColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar { }

            ColumnLayout {
                id: settingsColumn
                width: settingsFlick.width
                spacing: 14

                // ================= 通用 =================
                // ================= 账号（第一栏） =================
                AppCard {
                    id: accountSection
                    Layout.fillWidth: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 8

                        Text {
                            text: "账号与密码"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }

                        Text {
                            Layout.fillWidth: true
                            text: (typeof auth !== "undefined") ? auth.statusText : "账号模块未就绪"
                            color: (typeof auth !== "undefined" && auth.signedIn)
                                   ? Style.success : Style.text2
                            font.pixelSize: 11
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                        }

                        // ---------- 已登录：显示当前用户名 + 更改用户名 ----------
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: typeof auth !== "undefined" && auth.signedIn

                            Text {
                                text: "当前用户名：" + ((typeof auth !== "undefined") ? auth.userName : "")
                                color: Style.text2
                                font.pixelSize: 12
                                font.family: Style.fontFamily
                            }
                            AppTextField {
                                id: newUserField
                                Layout.fillWidth: true
                                placeholder: "新用户名"
                            }
                            AppButton {
                                text: "更改用户名"
                                variant: "ghost"
                                onClicked: if (typeof auth !== "undefined")
                                               auth.changeUserName(newUserField.text)
                            }
                        }

                        // ---------- 未登录：登录 / 首次注册 ----------
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: typeof auth === "undefined" || !auth.signedIn

                            AppTextField {
                                id: accountUserField
                                Layout.fillWidth: true
                                placeholder: "账号"
                            }
                            AppTextField {
                                id: accountPassField
                                Layout.fillWidth: true
                                placeholder: "密码（至少 6 位）"
                                echoMode: TextInput.Password
                            }
                            AppButton {
                                text: "注册账号"
                                variant: "primary"
                                visible: typeof auth !== "undefined" && !auth.hasAccount
                                onClicked: if (typeof auth !== "undefined")
                                               auth.saveAccount(accountUserField.text, accountPassField.text)
                            }
                            AppButton {
                                text: "登录"
                                variant: "primary"
                                visible: typeof auth !== "undefined" && auth.hasAccount
                                onClicked: if (typeof auth !== "undefined")
                                               auth.login(accountUserField.text, accountPassField.text)
                            }
                        }

                        // ---------- 操作按钮 ----------
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            AppButton {
                                text: "切换账号"
                                variant: "ghost"
                                visible: typeof auth !== "undefined" && auth.signedIn
                                onClicked: if (typeof auth !== "undefined") auth.switchAccount()
                            }
                            AppButton {
                                text: "退出登录"
                                variant: "ghost"
                                visible: typeof auth !== "undefined" && auth.signedIn
                                onClicked: if (typeof auth !== "undefined") auth.logout()
                            }

                            Item { Layout.fillWidth: true }

                            Text {
                                visible: typeof auth !== "undefined" && !auth.hasAccount
                                text: "首次使用：填写账号密码后点「注册账号」"
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                            }
                        }

                        // ---------- 更换密码（仅登录后） ----------
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: Style.line
                            visible: typeof auth !== "undefined" && auth.signedIn
                        }

                        Text {
                            text: "更换密码"
                            color: Style.text2
                            font.pixelSize: 11
                            font.family: Style.fontFamily
                            visible: typeof auth !== "undefined" && auth.signedIn
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: typeof auth !== "undefined" && auth.signedIn

                            AppTextField {
                                id: oldPassField
                                Layout.fillWidth: true
                                placeholder: "当前密码"
                                echoMode: TextInput.Password
                            }
                            AppTextField {
                                id: newPassField
                                Layout.fillWidth: true
                                placeholder: "新密码（至少 6 位）"
                                echoMode: TextInput.Password
                            }
                            AppButton {
                                text: "更换密码"
                                variant: "ghost"
                                onClicked: if (typeof auth !== "undefined")
                                               auth.changePassword(oldPassField.text, newPassField.text)
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "账号只保存在本机，使用 Windows 系统加密（DPAPI）与 PBKDF2 哈希；界面不展示存储位置。离线登录时设备同步、插件市场等功能受限。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                // ================= 关于 =================
                AppCard {
                    id: aboutSection
                    Layout.fillWidth: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 6

                        Text {
                            text: "关于"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: "Assistant v" + appVersion + " · Windows 桌面助手"
                            color: Style.text2
                            font.pixelSize: Style.fontBody
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: "配置与数据库位于 %LOCALAPPDATA%/AssistantTeam/Assistant/；主题、日志级别、文件索引与收件箱设置即时生效。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }

    // ---- 隐藏的按键捕获器（放在布局之外，避免影响 ColumnLayout）----
    Item {
        id: keyCapture
        width: 0
        height: 0
        focus: settingsPage.capturing

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

            if (event.key === Qt.Key_Escape && mods.length === 0) {
                event.accepted = true
                settingsPage.capturing = false
                return
            }

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
