import QtQuick
import QtQuick.Shapes
import ".."

// 单色线性图标：用 QtQuick.Shapes 直接画 SVG path，不依赖图片/字体。
//
// 为什么不用 emoji：不同 Windows 的字形、基线、颜色都不可控；
// 为什么不用 SVG 图片：还要额外引 Qt6::Svg，且染色麻烦。
//
// 用法：
//   AppIcon { name: "search"; size: 16; iconColor: Style.text2 }
Item {
    id: root

    property string name: "file"
    property color iconColor: Style.text2
    property int size: 16
    property real weight: 1.8
    property bool filled: false

    implicitWidth: size
    implicitHeight: size

    // 24×24 视图框；每个字符串可以包含多条子路径。
    readonly property var paths: ({
        "search":      "M11 4a7 7 0 1 0 0 14a7 7 0 0 0 0-14M16.2 16.2 21 21",
        "home":        "M3 10.5 12 3l9 7.5M5 9.5V21h14V9.5M9.5 21v-6h5v6",
        "todo":        "M6 4h12a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2M8.5 12l2.5 2.5 5-5",
        "note":        "M6 3h9l5 5v13H6zM15 3v5h5M9 13h6M9 17h4",
        "clip":        "M9 4H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V6a2 2 0 0 0-2-2h-2M9 3h6v3H9zM9 11h6M9 15h4",
        "download":    "M12 3v12M7 10l5 5 5-5M4 18v2h16v-2",
        "sync":        "M20 12a8 8 0 0 1-13.6 5.7L4 16M4 21v-5h5M4 12a8 8 0 0 1 13.6-5.7L20 8M20 3v5h-5",
        "plugin":      "M12 3 21 7.5v9L12 21 3 16.5v-9zM3 7.5l9 4.5 9-4.5M12 12v9",
        "settings":    "M4 21v-6M4 11V3M12 21v-4M12 13V3M20 21v-8M20 9V3M2 15h4M10 13h4M18 11h4",
        "filter":      "M3 4h18l-7 8v7l-4 2v-9z",
        "plus":        "M12 5v14M5 12h14",
        "arrowUp":     "M12 19V5M6 11l6-6 6 6",
        "arrowRight":  "M5 12h14M13 6l6 6-6 6",
        "enter":       "M20 5v6a3 3 0 0 1-3 3H5M9 10l-4 4 4 4",
        "file":        "M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9zM14 3v6h6M9 13h6M9 17h4",
        "folder":      "M3 7a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 0 1 2 2V18a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z",
        "app":         "M4 4h16v16H4zM3 9h18M7.5 6h1M10.5 6h1",
        "ppt":         "M3 4h18v13H3zM12 17v4M8 21h8",
        "calc":        "M5 3h14a1 1 0 0 1 1 1v16a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1M8 7h8M8 11h1M12 11h1M16 11h1M8 15h1M12 15h1M16 15h1M8 19h1M12 19h1M16 19h1",
        "close":       "M18 6 6 18M6 6l12 12",
        "minimize":    "M5 12h14",
        "maximize":    "M5 5h14v14H5z",
        "restore":     "M9 9h11v11H9zM5 15V4h11v5",
        "chevronDown": "M6 9l6 6 6-6",
        "chevronRight":"M9 6l6 6-6 6",
        "trash":       "M4 6h16M9 6V4h6v2M6 6l1 15h10l1-15M10 10v7M14 10v7",
        "check":       "M20 6 9 17l-5-5",
        "external":    "M14 4h6v6M20 4l-9 9M19 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1h5",
        "copy":        "M9 9h10v12H9zM5 15H4V3h10v1",
        "undo":        "M9 7 4 12l5 5M4 12h12a5 5 0 0 1 0 10h-3",
        "refresh":     "M20 12a8 8 0 0 1-13.6 5.7L4 16M4 21v-5h5M4 12a8 8 0 0 1 13.6-5.7L20 8M20 3v5h-5",
        "send":        "M21 3 3 10l7 3 3 7zM10 13l8-8",
        "inbox":       "M4 4h16v16H4zM4 14h5l2 3h2l2-3h5",
        "link":        "M10 14a5 5 0 0 0 7 0l2-2a5 5 0 0 0-7-7l-1 1M14 10a5 5 0 0 0-7 0l-2 2a5 5 0 0 0 7 7l1-1",
        "alert":       "M12 3 2 21h20zM12 9v5M12 17h.01",
        "info":        "M12 3a9 9 0 1 0 0 18a9 9 0 0 0 0-18M12 11v6M12 7h.01",
        "eye":         "M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12M12 9a3 3 0 1 0 0 6a3 3 0 0 0 0-6",
        "lock":        "M6 10h12v10H6zM9 10V7a3 3 0 0 1 6 0v3",
        "history":     "M3 12a9 9 0 1 0 3-6.7L3 8M3 3v5h5M12 7v5l3 2",
        "grid":        "M3 3h8v8H3zM13 3h8v8h-8zM3 13h8v8H3zM13 13h8v8h-8z",
        "chart":       "M4 20V10M10 20V4M16 20v-7M22 20H2",
        "sun":         "M12 8a4 4 0 1 0 0 8a4 4 0 0 0 0-8M12 2v2M12 20v2M4 12H2M22 12h-2M5.6 5.6 4.2 4.2M19.8 19.8l-1.4-1.4M5.6 18.4l-1.4 1.4M19.8 4.2l-1.4 1.4",
        "moon":        "M20 14.5A8 8 0 0 1 9.5 4 8.5 8.5 0 1 0 20 14.5",
        "zap":         "M13 2 4 14h7l-1 8 9-12h-7z",
        "play":        "M8 5v14l11-7z",
        "key":         "M14 7a4 4 0 1 1-3.5 5.9L9 14.5H7v2H5v2H2v-3l7.1-7.1A4 4 0 0 1 14 7",
        "shield":      "M12 3 20 6v6c0 5-4 8-8 9-4-1-8-4-8-9V6z",
        "device":      "M4 5h16v11H4zM8 20h8M12 16v4",
        "wifi":        "M2 8a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0M12 19h.01"
    })

    readonly property string path: paths[name] !== undefined ? paths[name] : paths["file"]

    Shape {
        visible: root.name !== "more"
        width: 24
        height: 24
        scale: root.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        layer.enabled: true
        layer.samples: 4

        ShapePath {
            strokeColor: root.iconColor
            strokeWidth: root.filled ? 0 : root.weight * 24 / root.size
            fillColor: root.filled ? root.iconColor : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.path }
        }
    }

    // “更多”是三个实心点，单独画，避免零长度子路径的渲染差异。
    Row {
        visible: root.name === "more"
        anchors.centerIn: parent
        spacing: root.size * 0.17
        Repeater {
            model: 3
            Rectangle {
                width: Math.max(2, root.size * 0.13)
                height: width
                radius: width / 2
                color: root.iconColor
            }
        }
    }
}
