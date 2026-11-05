// API 金丝雀：把 `host` 的**每一个**属性和**每一个**方法都用一遍。
//
// 为什么要有它：PluginHost 是唯一对外承诺的面（docs/plugins/README.md），
// 但它是普通 C++ 类 —— 删个属性、改个返回类型，编译照样过，只有外部插件会坏。
// 这个文件让"改坏了"变成 tests/ 里的一条红。
//
// 约定：**新增 host 能力时必须在这里补一行**，否则新能力没有回归保护。
//
// 注意：要求文件（plugin.json）里的 main 指向它；测试会把它复制到临时目录再加载，
// 因为 PluginHost 会在插件目录下建 data/，不能在仓库里留下写入痕迹。

import QtQml

QtObject {
    // ---- 属性快照（load() 返回后由测试读取）----
    property string seenId: ""
    property string seenName: ""
    property string seenVersion: ""
    property string seenAppVersion: ""
    property string seenDataDir: ""
    property bool   seenDark: false
    property int    seenApi: -1

    // ---- 方法结果 ----
    property string fallbackSeen: ""
    property bool   setSettingOk: false
    property string readBack: ""
    property bool   openEmptyRefused: false
    property bool   openOutsideRefused: false
    property string lastToast: ""

    // 测试通过 invokeMethod 调它，验证 toast 通道（onCompleted 阶段宿主还没接线）
    function fireToast(text) {
        lastToast = text
        host.toast(text)
    }

    function readSetting(key, fallback) {
        return host.setting(key, fallback)
    }

    Component.onCompleted: {
        // 属性
        seenId          = host.pluginId
        seenName        = host.pluginName
        seenVersion     = host.version
        seenAppVersion  = host.appVersion
        seenDataDir     = host.dataDir
        seenDark        = host.isDark
        seenApi         = host.apiVersion

        // log：四个级别都要能吞，不能抛
        host.log("debug", "canary: debug level")
        host.log("info",  "canary: info level")
        host.log("warn",  "canary: warn level")
        host.log("error", "canary: error level")

        // setting / setSetting 往返
        fallbackSeen = host.setting("never-written", "FALLBACK")
        setSettingOk = host.setSetting("canaryKey", "canaryValue")
        readBack     = host.setting("canaryKey", "")

        // openPath：只验"该拒的必须拒"。成功路径会真的拉起系统程序，测试不碰。
        openEmptyRefused   = (host.openPath("") === false)
        openOutsideRefused = (host.openPath("C:/Windows/win.ini") === false)
    }
}
