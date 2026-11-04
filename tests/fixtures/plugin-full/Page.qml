// 金丝雀的页面：验证 contributes.pages 能被装载、且页面里同样拿得到 host。
// 用 QtObject 而不是可视元素，是为了让测试在无 GUI 环境下也能跑（不渲染）。
import QtQml

QtObject {
    property string marker: "canary-page"
    property int apiSeen: host.apiVersion
    property string ownerId: host.pluginId
}
