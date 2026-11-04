# Assistant 插件开发指南

> 面向：想给 Assistant 写插件的人（第三方开发者）
> 适用宿主：Assistant `0.3.x`
> 接口版本：`manifestVersion = 1`
> 文档状态：**预览**。插件装载、页面贡献、本地/侧载安装已经可用；公共市场索引尚未上线（宿主默认只带示例数据）。

---

## 0. 30 秒了解

Assistant 的插件是一个 **文件夹**（或一个同样内容的 zip）：

```
my-plugin/
├─ plugin.json      ← 清单，宿主靠它认识你
├─ main.qml         ← 入口，插件加载时被实例化
└─ Page.qml         ← 贡献给左侧导航的页面（可选）
```

插件用 **QML** 写，在宿主进程内运行，通过一个叫 `host` 的对象与宿主交互。没有 C++、没有编译、没有依赖安装——改完 QML 重启宿主就能看到。

```
┌────────────── Assistant 进程 ──────────────┐
│  QML 引擎                                  │
│   ├─ 宿主界面（搜索浮窗 / 主面板）          │
│   └─ 你的插件（独立 QQmlContext）           │
│        └─ 唯一可见的宿主对象：host          │
└────────────────────────────────────────────┘
```

---

## 1. 快速开始

### 1.1 最小插件

`plugin.json`：

```json
{
  "manifestVersion": 1,
  "id": "com.example.hello",
  "name": "Hello",
  "version": "1.0.0",
  "description": "我的第一个 Assistant 插件",
  "author": "你的名字",
  "main": "main.qml"
}
```

`main.qml`：

```qml
import QtQuick

Item {
    Component.onCompleted: host.log("info", "hello 插件已加载，宿主版本 " + host.appVersion)
}
```

### 1.2 放到哪里

开发期直接放目录（不用打包，不用装）：

```
%LOCALAPPDATA%\AssistantTeam\Assistant\plugins\com.example.hello\
```

宿主启动时会扫描 `plugins\` 下所有**不以 `.` 开头**的目录，把没登记过的插件登记为"本地侧载"，然后重启 Assistant 就能在「插件市场 → 已安装」里看到它。

> 目录名必须等于 `plugin.json` 里的 `id`。不一致时宿主以目录里的清单为准重新登记，但以后升级/卸载会对不上，别这么干。

### 1.3 加载与验证

1. 重启 Assistant（**没有热重载**，见 §11）；
2. 打开主面板 → 左侧「插件市场」；
3. 卡片出现即装载成功；带红色「不可用」标签说明装载失败；
4. 看日志确认：

```
%LOCALAPPDATA%\AssistantTeam\Assistant\logs\assistant.log
```

里面会有 `PluginLoader: loaded com.example.hello (...)` 和你的 `plugin[com.example.hello]: ...` 输出。

### 1.4 打成 zip 侧载

把**插件目录的内容**（不是目录本身）压成 zip，保持 `plugin.json` 在压缩包根：

```
hello-1.0.0.zip
├─ plugin.json
├─ main.qml
└─ Page.qml
```

在「插件市场」里用「从文件安装」选择这个 zip 即可。安装后插件默认**不启用**，需要手动打开开关——这是有意的：先落盘、再启用，装坏了不会连带把宿主拖进循环。

---

## 2. 包结构

| 路径 | 必须 | 说明 |
|---|---|---|
| `plugin.json` | ✅ | 清单，必须在包根 |
| `main.qml`（或 `main` 指向的文件） | ✅ | 入口，加载时实例化 |
| `contributes.pages[].source` 指向的文件 | 可选 | 导航页 |
| `icon.png` 等 | 可选 | 清单里的 `icon` 字段只接受**字符/emoji**，图片图标目前不显示 |
| `data/` | 宿主创建 | 你的可写目录，**不要**自己打包进来 |

包内允许任意其他文件（图片、其它 `.qml`、`.js`）。宿主只按清单引用，不会执行未被引用的东西。

**不要往插件目录里写东西。** 升级时整个目录会被替换。要持久化就用 `host.setting()` 或写 `host.dataDir`。

---

## 3. `plugin.json` 参考

### 3.1 完整示例

```json
{
  "manifestVersion": 1,
  "id": "com.example.weather",
  "name": "天气",
  "version": "1.2.0",
  "description": "输入 w 北京 查看天气",
  "author": "your-name",
  "homepage": "https://github.com/you/assistant-weather",
  "license": "MIT",
  "icon": "🌤",
  "runtime": "qml",
  "engines": { "assistant": ">=0.3.0 <1.0.0", "api": ">=1.0.0" },
  "main": "main.qml",
  "permissions": ["network"],
  "contributes": {
    "pages": [
      { "id": "weather", "title": "天气", "icon": "🌤", "source": "Page.qml" }
    ]
  }
}
```

### 3.2 字段表

| 字段 | 必填 | 规则 |
|---|---|---|
| `manifestVersion` | ✅ | 必须是 `1`。将来出新格式会递增 |
| `id` | ✅ | `^[a-z0-9][a-z0-9._-]{2,63}$`，建议反向域名。**装了就不能改** |
| `name` | ✅ | 显示名 |
| `version` | ✅ | 严格 SemVer：`主.次.修订`，可带 `-预发布`（如 `1.2.0-beta.1`） |
| `description` | 建议 | 卡片上的一行摘要 |
| `author` / `homepage` / `license` | 可选 | 展示与溯源 |
| `icon` | 可选 | 单个 emoji 或字符（如 `"🌤"`），**不是**文件路径 |
| `runtime` | 可选 | 默认 `"qml"`。`"native"` 目前会被**拒绝安装** |
| `engines.assistant` | 建议 | 兼容的宿主版本区间；省略表示"未声明"（只警告不拒绝） |
| `engines.api` | 建议 | 需要的**插件 API 版本**区间（如 `">=1.0.0"`）；省略表示不声明。与 `engines.assistant` 是两条独立的轴，见 §7.2 |
| `main` | ✅（qml） | 入口文件，插件内相对路径 |
| `permissions` | 建议 | 字符串数组，**仅作声明展示**（见 §9） |
| `contributes.pages` | 可选 | 见 §5 |
| `contributes.commands` | 可选 | **已解析、尚未生效**（§6） |
| `contributes.searchProviders` | 可选 | **已解析、尚未生效**（§6） |
| `contributes.settings.source` | 可选 | **已解析、尚未生效**（§6） |

### 3.3 会被直接拒绝的情况

清单校验是**严格**的：任何一条不过，插件不会进入登记表，`detail` 里会给出原因。

- 不是合法 JSON / 根不是对象
- `manifestVersion` ≠ 1
- `id` 不符合正则（大写、`@`、`/`、长度不足）
- `version` 不是严格 SemVer
- `runtime` 不是 `qml` / `native`
- `runtime = "qml"` 但缺 `main`
- `main` 或任意 `source` **路径逃逸**：`..`、反斜杠、以 `/` 开头、带盘符（`C:`）
- `contributes.pages` 里 `id` 重复、缺 `title`、`source` 逃逸

---

## 4. 宿主 API：`host`

每个插件拿到**自己专属**的一个 `host` 实例，作为 QML context property 注入。你只能看到这一层——拿不到宿主的注册表、管理器、引擎。

### 4.1 属性

| 属性 | 类型 | 说明 |
|---|---|---|
| `host.pluginId` | string | 你自己的 id |
| `host.pluginName` | string | 清单里的 `name` |
| `host.version` | string | 你自己的版本 |
| `host.appVersion` | string | 宿主版本（如 `0.3.0`），做版本判断用 |
| `host.apiVersion` | int | 宿主实现的**插件 API 版本**（当前 `1`）。判断"这个宿主支不支持我要用的方法"用它，不是用 `appVersion` |
| `host.dataDir` | string | 你的可写目录绝对路径（首次写时才创建） |
| `host.isDark` | bool | 当前是否是深色主题；主题切换时触发 `themeChanged` |

### 4.2 方法

| 方法 | 返回 | 说明 |
|---|---|---|
| `host.log(level, message)` | — | `level` ∈ `debug` / `info` / `warn` / `error`（其它值按 `info`）。写进宿主日志，前缀 `plugin[<你的 id>]` |
| `host.setting(key, fallback)` | any | 读你自己的持久化键；不存在返回 `fallback` |
| `host.setSetting(key, value)` | bool | 写持久化键。**返回 false 表示写盘失败，且内存值已回滚** |
| `host.toast(text)` | — | 请宿主弹一条短提示（宿主决定怎么显示） |
| `host.openPath(path)` | bool | 打开 https / http / mailto，或**你自己 data 目录内**的文件；其它一律拒绝并返回 false |

持久化落在 `host.dataDir/settings.json`，整文件原子写。支持的值类型：bool、整数、double、string、字符串数组、以及嵌套的 list/map（会被转成 JSON）。

### 4.3 示例

```qml
import QtQuick

Item {
    function bumpCount() {
        var n = host.setting("runs", 0) + 1
        if (!host.setSetting("runs", n))
            host.log("warn", "计数保存失败（磁盘只读？）")
        return n
    }

    Component.onCompleted: {
        host.log("info", "第 " + bumpCount() + " 次加载，宿主 " + host.appVersion)
        if (host.isDark) host.toast("深色模式")
    }
}
```

### 4.4 宿主不会给你的东西

这不是"还没做"，是**设计边界**（`插件市场-模块框架.md` §4.5）：

- 宿主的内部对象：注册表、管理器、QML 引擎——一个都不给；
- 任意文件读写 API：只有你自己的 `data/`；
- 任意程序启动：`openPath` 只放行 `https/http/mailto` 和 data 目录；
- 宿主的 `Style` 单例（见 §5.4）。

将来如果要支持更强的能力（真正的文件访问、网络请求封装），会以**新方法**的形式加在这一层，而不是把内部对象暴露出来——否则"以后把插件挪到进程外"就永久不可能了。

---

## 5. 贡献页面

### 5.1 清单

```json
"contributes": {
  "pages": [
    { "id": "weather", "title": "天气", "icon": "🌤", "source": "Page.qml" }
  ]
}
```

装载成功后，左侧导航会**立刻**出现「天气」一项，排在「插件市场」之后。停用或卸载插件时自动消失。

### 5.2 `Page.qml` 约定

- 根元素建议用 `Rectangle` 或 `Item`，它会**填满**内容区，不要自己设 `width`/`height` 或锚到 `parent` 的父级；
- 页面在你的 `main.qml` 的**同一个 QML context** 里创建，所以 `host` 在这里照样可用；
- 不要依赖宿主的 import 路径，只 `import QtQuick` / `QtQuick.Controls` / `QtQuick.Layouts`；
- 你被创建时可能还没有父对象，别假设 `parent` 存在。

```qml
import QtQuick
import QtQuick.Layouts

Rectangle {
    readonly property bool dark: host.isDark
    color: dark ? "#2c2c2c" : "#f0f0f0"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Text {
            text: "天气"
            font.pixelSize: 18
            font.bold: true
            color: dark ? "#e0e0e0" : "#1a1a1a"
        }
        Text {
            text: "宿主 " + host.appVersion
            color: dark ? "#666666" : "#999999"
        }
    }
}
```

### 5.3 生命周期与缓存

| 事件 | 你会经历什么 |
|---|---|
| 插件启用 | `main.qml` 被实例化一次（`Component.onCompleted`） |
| 首次点开页面 | 对应的 `Page.qml` 才被创建（**懒加载**） |
| 切走再切回 | 页面实例被复用，**不会**重建（状态保留） |
| 插件停用 | 页面与入口对象一起销毁 |
| 重新启用 | 全部重建，内存状态清零；`settings.json` 还在 |

### 5.4 主题与配色（没有 `Style` 单例）

宿主的 `Style.qml` 是 `qrc` 里的 QML 单例，你的插件在 `file://` 目录下，import 路径不同，**拿不到它**。自己配色，用 `host.isDark` 判断，宿主的色板如下（建议照着来，观感才一致）：

| 用途 | 深色 | 浅色 |
|---|---|---|
| 页面背景 | `#2c2c2c` | `#f0f0f0` |
| 卡片/面板 | `#1e1e1e` | `#ffffff` |
| 主文字 | `#e0e0e0` | `#1a1a1a` |
| 次文字 | `#666666` | `#999999` |
| 强调（选中/按钮） | `#3a6ea5` | `#4a90d9` |
| 边框 | `#3d3d3d` | `#d0d0d0` |
| 错误 | `#e0716b` | `#d0453c` |

---

## 6. 尚未启用的贡献点

诚实地说清楚，免得你按现在的文档写了却等不到效果：

| 贡献点 | 现状 | 预计 |
|---|---|---|
| `contributes.commands` | 清单**解析并保存**，但没有任何消费方——命令不会出现在搜索框 | 等搜索融合层（文件子系统 A7）落地后 |
| `contributes.searchProviders` | 同上，且需要新的 `ISearchProvider` 扩展点 | 同上 |
| `contributes.settings.source` | 解析，没有设置页入口 | 后续版本 |

现在能用的是：**导航页**（`contributes.pages`）+ `host` 的全部能力。

---

## 7. 版本与兼容

### 7.1 `engines.assistant`

宿主启动时会把你的版本区间和 `appVersion` 比对：

- 区间**不满足** → 插件拒绝启用，卡片上打「需 ≥x.y.z」标签；
- **不声明**（字段缺省）→ 只写一条警告日志，照常启用。

支持的区间语法（子集，不是完整 SemVer）：

| 写法 | 含义 |
|---|---|
| `>=0.3.0 <1.0.0` | 空格分隔 = AND |
| `>=0.3.0 \|\| >=0.4.0` | `\|\|` = OR |
| `^0.3.0` | `>=0.3.0 <0.4.0`（0.x 时次版本进位） |
| `~0.3.2` | `>=0.3.2 <0.4.0` |
| `0.3.x`、`1.2.x` | 通配 |
| `=0.3.0` | 精确 |

**不支持**：`*`、`latest`、URL、预发布排除规则。写 `*` 会被判为非法区间。

### 7.2 `engines.api`（插件 API 版本）

不要让 `engines.assistant` 兼职管这件事。两条轴的含义不同：

| 轴 | 回答的问题 | 例子 |
|---|---|---|
| `engines.assistant` | "这个宿主**产品**够新吗"（界面/行为） | 宿主要 0.4 才有某个菜单 |
| `engines.api` | "这个宿主**插件 API** 够新吗"（能力面） | 宿主会不会给我 `host.xxx()` 这个新方法 |

当前 API 版本是 **1**（`host.apiVersion`），比较时按 `1.0.0` 处理：

```json
"engines": { "assistant": ">=0.3.0 <1.0.0", "api": ">=1.0.0" }
```

规则：

- 不声明 `api` → 不检查（和 `assistant` 一样，缺省不等于不兼容）；
- 区间语法与 §7.1 完全一致，写 `*` 会被判非法；
- 不满足 → 插件**拒绝启用**，卡片上标 `不可用`，`detail` 说明要求的 API 版本。

**什么时候该写它**：你用了某个"较新才加进来"的 host 方法。宿主新增 API 时只升 API 版本、**不**升 `manifestVersion`，所以 `api` 是唯一能提前挡住"宿主太旧、方法不存在"的声明。

### 7.3 升级

- 插件没有自动更新。升级 = **卸载旧版 + 装新版**；
- 装完必须**重启宿主**才生效（QML 组件缓存 + 已实例化对象，见 §11）；
- `data/` 目录在升级时保留，`settings.json` 不丢。

---

## 8. 打包与校验

安装器（`PluginInstaller`）对一个 zip 做的事，按顺序：

1. 解压到临时目录，**逐个 entry 校验路径**：绝对路径、盘符、`..`、空段一律拒绝（zip-slip 防护）；
2. 检查根有 `plugin.json`，能解析、能通过 §3.3 的全部校验；
3. 检查 `runtime`：`native` 直接拒绝；
4. 检查 `version` 与索引声明的版本**一致**（防"索引说 1.2.0、包里是 1.1.0"）；
5. 检查 `engines.assistant`；
6. 原子提交到 `plugins\<id>\`，写 `plugins.json`；
7. 启用：装载 `main.qml`。装载失败**不影响安装结果**，只打「不可用」标签（文件保留，可卸载）。

从 URL 安装时还会比对清单里的 `sha256`（不匹配 → `checksum` 错误，不落盘）。

---

## 9. 安全模型（务必读）

**你的插件和宿主在同一个进程里跑，拥有和宿主一样的权限。**

- ❌ 没有沙箱
- ❌ 没有权限拦截：`permissions` 只用于**安装前展示**和排障，宿主**不会**因为没声明就阻止你做什么
- ❌ 没有签名校验（v1）：宿主只校验 `sha256`，它证明"包没被改过"，不证明"作者是谁"
- ✅ 你的崩溃会带走整个 Assistant

所以：

1. 装插件的人应该只装自己信任的来源——这句会写在安装确认里；
2. 你自己也别写死循环（`while(true)` 会卡住 UI 线程）；
3. 需要用户确认的危险动作，用 `host.toast()` + 插件页面里的二次确认，别偷偷做。

---

## 10. 错误码速查

装载/安装失败时，卡片和安装结果里会给一个 `code`：

| code | 含义 | 你能做什么 |
|---|---|---|
| `invalid-manifest` | `plugin.json` 校验失败，`detail` 里有具体原因 | 按 §3.3 改清单 |
| `load-failed` | QML 编译或实例化失败，`detail` 里是 Qt 的报错（含文件:行:列） | 看 `detail` 修 QML |
| `not-a-plugin` | 包里没有 `plugin.json` | 检查压缩包是不是多套了一层目录 |
| `version-mismatch` | 包内版本 ≠ 索引声明 | 重新打包 |
| `incompatible-version` | `engines.assistant` 不满足当前宿主 | 放宽区间或升级宿主 |
| `checksum` | sha256 不匹配 | 重新下载/重新打包 |
| `zip-slip` | 压缩包里有越界路径 | 重新打包（别带 `..`、别带绝对路径） |
| `already-installed` | 同版本已装 | 先卸载 |
| `permission` | 写盘被拒 | 检查目录权限 / 杀软 |
| `disk-full` | 磁盘满 | 清理 |
| `network` / `http-status` | 下载失败 | 重试或换源 |
| `not-found` | 索引里没有这个 id | 检查 id 拼写 |
| `cancelled` / `too-late` | 用户取消；或取消时已进入提交阶段 | 重装 |
| `not-removable` | 内置插件 | 只能停用，不能卸载 |

---

## 11. 调试

| 手段 | 怎么做 |
|---|---|
| 日志 | `%LOCALAPPDATA%\AssistantTeam\Assistant\logs\assistant.log`，搜 `plugin[<你的 id>]` |
| 状态文件 | `plugins\plugins.json`——你的 `enabled` / `problem` 都在这里，可直接手改（改完重启） |
| 强制重扫 | 删掉 `plugins.json` 里你那条记录，重启 → 宿主会重新扫目录并登记为本地插件 |
| 卸载残留 | 卸载是"移到 `.trash\`"，删失败不会报错但目录还在，下次启动重试 |

**没有热重载**：改 `.qml` 必须重启 Assistant。这是已知限制（QML 组件缓存 + 已实例化对象，同进程替换不可靠），不是 bug。

---

## 11.5 示例与工具（在仓库里）

| 东西 | 位置 | 用途 |
|---|---|---|
| **参考插件** | `examples/text-tools/` | 纯本地、零权限的最小可用插件：文本大小写/去重/统计。抄它最快 |
| **生成骨架** | `tools/plugin-new.ps1` | `-Id com.me.hello -Name "Hello"` → 出清单 + 入口 + 页面 + README |
| **打包 + 算摘要** | `tools/plugin-pack.ps1` | 校验清单 → 打 zip（根目录是 `plugin.json`）→ 输出 sha256 与市场索引条目 |
| **API 金丝雀** | `tests/fixtures/plugin-full/` | 用到 `host` **每一个**属性与方法；改 `PluginHost` 时它会变红 |

两个脚本在本机默认被执行策略挡住，用这个方式跑：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\plugin-pack.ps1 -PluginDir .\examples\text-tools -OutDir .\dist
```

**给宿主开发者的一条义务**：动 `PluginHost`（加/删/改属性或方法）时，必须同步改
`tests/fixtures/plugin-full/main.qml` 并在 `tests/` 里跑一遍——否则新能力没有回归保护，
旧能力被改坏也没人发现。接口对外承诺的前提是这条能守住。

---

## 12. 发布到市场（当前还不存在）

宿主这边的协议已经定了：一个静态 `index.json` + 一堆 zip。但**公共索引还没有**，默认用的是编译进程序的示例数据（界面上标着"示例数据 · 安装功能尚未接入真实市场"）。

自己测的时候可以指向本地索引：

```powershell
$env:ASSISTANT_MARKET_URL = "http://127.0.0.1:8000/index.json"
```

索引条目形如：

```json
{
  "schemaVersion": 1,
  "generatedAt": "2026-11-01T00:00:00Z",
  "plugins": [
    {
      "id": "com.example.hello",
      "name": "Hello",
      "summary": "一句话介绍",
      "description": "长描述（Markdown，暂未渲染）",
      "author": "your-name",
      "categories": ["工具"],
      "tags": ["官方"],
      "icon": "🌤",
      "latest": {
        "version": "1.0.0",
        "url": "https://example.com/hello-1.0.0.zip",
        "sha256": "<64 位小写十六进制>",
        "size": 12345,
        "minAppVersion": "0.3.0",
        "changelog": "首个版本"
      },
      "updatedAt": "2026-11-01T00:00:00Z"
    }
  ]
}
```

公开发布渠道确定后会更新这一节。

---

## 13. 接口变更记录

| 日期 | manifestVersion | API 版本 | 变更 |
|---|---|---|---|
| 2026-10 | 1 | 1 | 首版：`host` 6 属性 5 方法；页面贡献；zip 安装；`commands`/`searchProviders`/`settings` 保留未启用 |
| 2026-11 | 1 | 1 | 新增 `host.apiVersion` 与 `engines.api`（两条轴分离）；契约常量落在 `include/contract/IHostApi.h` |

**兼容承诺（预览期）**：`manifestVersion = 1` 下，`host` 的属性/方法只会**新增**，不会删除或改变语义。真要改语义会升 `manifestVersion`，旧插件会被拒绝加载并提示原因；只新增能力则升 `apiVersion`（插件用 `engines.api` 声明需求）。
