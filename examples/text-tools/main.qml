// 示例插件的入口对象。
//
// main.qml 在插件启用时**实例化一次**，之后一直存在（页面是另外懒加载的）。
// 这里只做两件事：记一条日志、记一次使用次数（演示 host.setting 持久化）。
//
// 用 Item 而不是 QtObject：插件入口如果是可视元素也没问题，宿主不会把它显示出来；
// 但测试环境是无 GUI 的，所以这个示例统一用 QtObject 更保险。
import QtQml

QtObject {
    Component.onCompleted: {
        var runs = host.setting("runs", 0) + 1
        if (!host.setSetting("runs", runs))
            host.log("warn", "使用次数保存失败（插件目录不可写？）")

        host.log("info", "文本工具已加载（第 " + runs + " 次），宿主 " + host.appVersion
                 + "，插件 API " + host.apiVersion)
    }
}
