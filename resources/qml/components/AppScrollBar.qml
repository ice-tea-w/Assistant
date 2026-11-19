import QtQuick
import QtQuick.Controls
import ".."

// 细滚动条：6px，悬停 8px；只画滑块，不画轨道。
ScrollBar {
    id: control

    policy: ScrollBar.AsNeeded
    minimumSize: 0.08
    implicitWidth: 8

    contentItem: Rectangle {
        width: 6
        height: {
            var track = control.parent ? control.parent.height : 200
            return Math.max(24, control.size * track)
        }
        y: {
            var track = control.parent ? control.parent.height : 200
            return control.position * Math.max(0, track - height)
        }
        radius: 3
        visible: control.size < 1.0 && (control.active || control.hovered)
        color: control.pressed ? Style.accent
             : control.hovered ? Style.lineStrong
             : Style.alpha(Style.text3, 0.35)
        Behavior on color { ColorAnimation { duration: Style.motionFast } }
    }

    background: Item { }
}
