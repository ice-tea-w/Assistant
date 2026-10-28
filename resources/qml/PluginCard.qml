import QtQuick
import QtQuick.Layouts

// 插件卡片（插件市场 M0）
//
// 约束（来自 docs/architecture/插件市场功能设计.md §9.2）：
//   1. 高度**固定 64px** —— 状态变化（进度条出现/角标增减）不得让列表跳动；
//   2. 本组件**只有信号没有动作** —— 点击一律 emit，由 PluginsPage 转给 bridge；
//   3. 视觉完全由 `state` 决定，QML 里不写"能不能装"这类业务判断。
//
// state 取值与 LLD §9.3 一一对应：
//   not-installed / installing / installed / update-available /
//   incompatible / problem / bundled
Rectangle {
    id: card

    // ---- 数据（由 PluginsPage 从 modelData 灌入）----
    property string pluginId: ""
    property string name: ""
    property string version: ""
    property string summary: ""
    property string icon: ""
    property string state: "not-installed"
    property string badge: ""
    property string badgeKind: ""        // accent | gray | orange | red
    property string problem: ""
    property int    percent: 0
    property bool   pluginEnabled: true
    /// 内置示例数据：下载地址是占位符，安装功能还没接上真实市场。
    property bool   demo: false

    signal installClicked()
    signal cancelClicked()
    signal toggleToggled(bool want)
    signal uninstallClicked()
    signal detailClicked()
    signal updateClicked()

    readonly property bool isProblem: state === "problem"
    readonly property bool isInstalling: state === "installing"
    readonly property bool isInstalled: state === "installed"
                                  || state === "update-available"
                                  || state === "bundled"
    readonly property bool isBundled: state === "bundled"
    readonly property bool isIncompatible: state === "incompatible"
    readonly property bool dimmed: state === "installed" && !pluginEnabled

    // ---- 视觉 ----
    height: 64
    radius: Style.radius
    color: isProblem
           ? (Style.isDark ? "#2c2224" : "#fefafa")
           : Style.panelColor
    border.width: 1
    border.color: isProblem ? Style.dangerColor : Style.borderColor
    opacity: dimmed ? 0.62 : 1.0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    readonly property color textColor: Style.textColor
    readonly property color mutedColor: Style.mutedColor

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 10

        // ---- 图标 ----
        Text {
            Layout.preferredWidth: 26
            text: card.isProblem ? "⚠" : (card.icon !== "" ? card.icon : "🧩")
            font.pixelSize: 20
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        // ---- 名称 / 版本 / 角标 + 副标题 ----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3

            RowLayout {
                spacing: 6
                Layout.fillWidth: true

                Text {
                    text: card.name
                    color: card.textColor
                    font.pixelSize: Style.bodySize
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.maximumWidth: card.width * 0.42
                }

                Text {
                    text: "v" + card.version
                    color: card.mutedColor
                    font.pixelSize: 10
                }

                // 角标
                Rectangle {
                    visible: card.badge !== ""
                    height: 15
                    width: badgeText.implicitWidth + 12
                    radius: 8
                    color: {
                        switch (card.badgeKind) {
                        case "accent": return Style.isDark ? "#1e3a5c" : "#e8f1fb"
                        case "red":    return Style.isDark ? "#3d2422" : "#fdf0ef"
                        case "orange": return Style.isDark ? "#3d3222" : "#fdf3e2"
                        default:       return Style.isDark ? "#2a2a2a" : "#f0f0f0"
                        }
                    }

                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: card.badge
                        font.pixelSize: 9
                        color: {
                            switch (card.badgeKind) {
                            case "accent": return Style.isDark ? "#7ab0e8" : "#3a6ea5"
                            case "red":    return Style.dangerColor
                            case "orange": return Style.isDark ? "#e0b070" : "#b57a1e"
                            default:       return card.mutedColor
                            }
                        }
                    }
                }

                // 剩余空间推挤，保证右侧控件不越界
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                text: card.isProblem && card.problem !== "" ? card.problem
                                                            : card.summary
                color: card.isProblem ? Style.dangerColor : card.mutedColor
                font.pixelSize: 11
                elide: Text.ElideRight
                maximumLineCount: 1
            }
        }

        // ---- 右侧控件：按 state 切换 ----
        Item {
            Layout.preferredWidth: 116
            Layout.preferredHeight: 32

            // 未安装
            // demo（内置示例数据）时按钮置灰、文案改成"未接入"，
            // 让人一眼看出"这里还没做完"，而不是点下去等一句网络错误。
            Rectangle {
                visible: card.state === "not-installed" || card.isIncompatible
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: card.demo ? 62 : 56
                height: 24; radius: Style.radiusSmall
                color: (card.isIncompatible || card.demo)
                       ? (Style.isDark ? "#2f2f2f" : "#ececec")
                       : Style.accentColor
                border.width: card.demo ? 1 : 0
                border.color: Style.borderColor

                Text {
                    anchors.centerIn: parent
                    text: card.demo ? "未接入" : "安装"
                    color: (card.isIncompatible || card.demo) ? card.mutedColor
                                                              : Style.accentTextColor
                    font.pixelSize: 11
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: !card.isIncompatible && !card.demo
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: card.installClicked()
                }
            }

            // 安装中：细进度条 + 取消
            Row {
                visible: card.isInstalling
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    width: 56

                    Rectangle {
                        width: parent.width
                        height: 3
                        radius: 2
                        color: Style.isDark ? "#3a3a3a" : "#e6e6e6"

                        Rectangle {
                            width: parent.width * Math.max(0, Math.min(1, card.percent / 100))
                            height: parent.height
                            radius: 2
                            color: Style.accentColor
                            Behavior on width { NumberAnimation { duration: 150 } }
                        }
                    }

                    Text {
                        anchors.right: parent.right
                        text: card.percent + "%"
                        font.pixelSize: 9
                        color: Style.accentColor
                    }
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44; height: 24; radius: Style.radiusSmall
                    color: "transparent"
                    border.width: 1
                    border.color: Style.borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "取消"
                        color: card.textColor
                        font.pixelSize: 11
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.cancelClicked()
                    }
                }
            }

            // 有问题：卸载
            Rectangle {
                visible: card.isProblem
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 56; height: 24; radius: Style.radiusSmall
                color: "transparent"
                border.width: 1
                border.color: Style.dangerColor

                Text {
                    anchors.centerIn: parent
                    text: "卸载"
                    color: Style.dangerColor
                    font.pixelSize: 11
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.uninstallClicked()
                }
            }

            // 已安装：开关 + 详情
            Row {
                visible: card.isInstalled && !card.isProblem
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                // 手绘开关（对齐 TodoPage 的勾选框风格，不用 Controls.Switch）
                Rectangle {
                    id: switchTrack
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32; height: 18; radius: 9
                    color: card.pluginEnabled ? Style.accentColor
                                              : (Style.isDark ? "#4d4d4d" : "#cccccc")
                    Behavior on color { ColorAnimation { duration: 120 } }

                    Rectangle {
                        width: 14; height: 14; radius: 7
                        y: 2
                        x: card.pluginEnabled ? parent.width - 16 : 2
                        color: "#ffffff"
                        Behavior on x { NumberAnimation { duration: 120 } }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.toggleToggled(!card.pluginEnabled)
                    }
                }

                // 详情（内置插件也可看，但没有卸载）
                Item {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18; height: 18

                    Text {
                        anchors.centerIn: parent
                        text: "⋯"
                        color: detailMouse.containsMouse ? card.textColor : card.mutedColor
                        font.pixelSize: 20
                        font.bold: true
                    }

                    MouseArea {
                        id: detailMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.detailClicked()
                    }
                }
            }
        }
    }
}
