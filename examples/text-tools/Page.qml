// 示例插件贡献的页面：纯本地文本处理，不联网、不读写文件。
//
// 想抄的时候注意三件事（docs/plugins/README.md §5.2）：
//   1. 根元素填满内容区，不要自己设 width/height；
//   2. 只 import QtQuick / QtQuick.Controls / QtQuick.Layouts，别依赖宿主的 import 路径；
//   3. 配色自己来，用 host.isDark 判断（宿主的 Style 单例拿不到）。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    readonly property bool dark: host.isDark
    readonly property color fg:      dark ? "#e0e0e0" : "#1a1a1a"
    readonly property color muted:   dark ? "#666666" : "#999999"
    readonly property color bgColor: dark ? "#2c2c2c" : "#f0f0f0"
    readonly property color panel:   dark ? "#1e1e1e" : "#ffffff"
    readonly property color edge:    dark ? "#3d3d3d" : "#d0d0d0"
    readonly property color accent:  dark ? "#3a6ea5" : "#4a90d9"

    color: bgColor

    function transform(mode) {
        var text = input.text
        var lines = text.split("\n")
        var out = ""

        if (mode === "upper") {
            out = text.toUpperCase()
        } else if (mode === "lower") {
            out = text.toLowerCase()
        } else if (mode === "trim") {
            out = lines.map(function (line) { return line.trim() }).join("\n")
        } else if (mode === "dedupe") {
            // Map 而不是 {}：键里出现 "__proto__" 之类的行名时对象字面量会出事
            var seen = new Map()
            var kept = []
            for (var i = 0; i < lines.length; i++) {
                if (!seen.has(lines[i])) {
                    seen.set(lines[i], true)
                    kept.push(lines[i])
                }
            }
            out = kept.join("\n")
        } else if (mode === "reverse") {
            out = lines.slice().reverse().join("\n")
        }

        output.text = out
    }

    function counts() {
        var text = output.text
        if (text.length === 0)
            return "空"
        var lines = text.split("\n").length
        return lines + " 行 · " + text.length + " 字符"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "文本工具"
                color: fg
                font.pixelSize: 18
                font.bold: true
                Layout.fillWidth: true
            }

            Text {
                text: host.appVersion
                color: muted
                font.pixelSize: 11
            }
        }

        TextArea {
            id: input
            Layout.fillWidth: true
            Layout.fillHeight: true
            placeholderText: "在这里粘贴文本…"
            color: fg
            font.pixelSize: 13
            wrapMode: TextArea.WrapAnywhere
            background: Rectangle {
                color: panel
                radius: 6
                border.color: input.activeFocus ? accent : edge
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: [
                    { "id": "upper",   "label": "大写" },
                    { "id": "lower",   "label": "小写" },
                    { "id": "trim",    "label": "去首尾空白" },
                    { "id": "dedupe",  "label": "去重行" },
                    { "id": "reverse", "label": "行反转" },
                    { "id": "clear",   "label": "清空" }
                ]

                delegate: Rectangle {
                    width: 84
                    height: 30
                    radius: 6
                    color: hover.containsMouse ? accent : panel
                    border.color: edge

                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        color: hover.containsMouse ? "#ffffff" : fg
                        font.pixelSize: 12
                    }

                    MouseArea {
                        id: hover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (modelData.id === "clear") {
                                input.text = ""
                                output.text = ""
                            } else {
                                root.transform(modelData.id)
                            }
                        }
                    }
                }
            }
        }

        TextArea {
            id: output
            Layout.fillWidth: true
            Layout.fillHeight: true
            readOnly: true
            placeholderText: "结果…"
            color: fg
            font.pixelSize: 13
            wrapMode: TextArea.WrapAnywhere
            background: Rectangle {
                color: panel
                radius: 6
                border.color: edge
            }
        }

        Text {
            text: counts()
            color: muted
            font.pixelSize: 11
        }
    }
}
