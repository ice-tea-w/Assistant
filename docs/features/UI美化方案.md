# UI 美化方案 · 视觉方向 A「柔光玻璃」

> 状态：**已实施（2026-11-19，P0–P2；P3 未做）** · 视觉稿与代码以 `resources/qml/` 为准
> 视觉稿：[assistant-ui-direction-a.jpg](ui-preview/assistant-ui-direction-a.jpg)（深/浅两套，按真实窗口 1:1 排版）
> 可交互源文件：[mockup.html](ui-preview/mockup.html)（浏览器直接打开）
> 范围：只动 **QML 视觉层 + 组件层**；不碰桥接、数据库、检索与同步逻辑。

---

## 0. 结论先说

当前界面的问题不是"颜色不好看"，而是**没有设计系统**：

- 颜色、字号、间距、圆角散落在 11 个 QML 文件里，同一个"次要文字"有 5 种灰；
- 按钮/输入框/卡片/chip 都是各页手写的 `Rectangle + MouseArea`，hover / pressed / focus / disabled 四种状态基本没有；
- 深色主题的层级是反的（卡片比背景更暗），次要文字对比度只有 **2.9:1**；
- 图标全是 emoji，换台机器字形和基线就变；
- 主面板用系统原生边框，却又画了 12px 圆角，两种窗口语言打架。

所以方案分两层：**先立令牌和组件（P0/P1），再逐页换皮（P2）**。不追求"重做一遍 UI"，追求"同一套规则覆盖所有页面"。

推荐方向 **A 柔光玻璃**：深色优先、蓝紫点缀、层叠表面 + 细边框 + 柔和阴影。它和现有的蓝色强调色连续，不需要改窗口结构就能落地，也最适合"搜索工具"这种高频、短停留的场景。

---

## 1. 现状诊断（都有代码依据）

| # | 问题 | 证据 | 影响 |
|---|------|------|------|
| 1 | **深色层级反了** | `Style.qml:16-17`：`bgColor #2c2c2c`，`panelColor #1e1e1e`。卡片比窗口背景更暗 | 视觉上卡片"凹进去"，没有浮起感；和 Fluent 的 elevation 规则相反 |
| 2 | **次要文字不可读** | 深色 `mutedColor #666` on `#1e1e1e` ≈ **2.90:1**；浅色 `#999` on `#fff` ≈ **2.85:1** | 11–12px 的说明、时间、路径在真实屏幕上发灰，低于 WCAG AA 4.5:1 |
| 3 | **只有一套蓝，没有语义色** | `Style.qml` 仅 7 个颜色；文件/应用/计算器/URL 的色值硬编码在 `MainWindow.qml:57-65` | 成功/警告/危险/信息没有统一表达；搜索结果类型色和主题脱节 |
| 4 | **间距没有网格** | `MainWindow.qml:237` margins 10；`MainPanel.qml:186` margins 10；`SettingsPage.qml:45` margins 8；`RecentDownloadsPage.qml:43` margins 8；Todo/Notes/Clip 页 margins 0 | 标题到内容、卡片到边缘的距离每页都不一样；窗口一小就显挤 |
| 5 | **字号层级弱且重复声明** | `Style.qml` 只有 18/13/11；页面里另有 10/12/14/16 硬编码；每个页面各自声明 5–6 个 `readonly property color` | 改一次主题要改 8 个文件；标题、正文、说明的对比不够明显 |
| 6 | **组件不可复用** | 按钮 = `Rectangle + Text + MouseArea`（Todo/Notes/Clip/Settings 各写一遍）；输入框同理 | 没有统一的 hover/pressed/focus/disabled；改一处不会同步到别处 |
| 7 | **缺少交互状态** | 自定义按钮只有 `onClicked`，绝大多数没有 hover；`TodoPage.qml:88-108` 删除按钮常显；`ClipboardPage.qml:95` hover 时整行铺满 accent | 不知道哪里可以点；误删风险；hover 过重、抢视线 |
| 8 | **emoji 当结构图标** | 🏠 📝 📋 🔄 🧩 ✓ ⚙ ⬇，以及 `⌕ ▤ ▶ # ↗ ⚡`（`MainWindow.qml:47-55`、`MainPanel.qml:70-77`） | 不同 Windows/字体渲染不一致，颜色不可控，基线和尺寸对不齐 |
| 9 | **两套窗口语言** | `MainWindow.qml:10` 是 Frameless + 12px 圆角；`MainPanel.qml` **没有设** `FramelessWindowHint`，却有 12px 圆角自绘背景 | 主面板顶部是系统标题栏，下面是圆角内容，接缝和配色都不统一；关闭/最小化交互也不同 |
| 10 | **设置页是一根长滚动** | `SettingsPage.qml` 814 行、单列 `ColumnLayout`，没有分组锚点 | 找"文件索引范围"要滚很久；加功能只会更长 |
| 11 | **空状态/危险操作弱** | 空列表只有一行灰字（Todo/Clip/Downloads）；剪贴板"清空"无确认；删除用灰色方块 | 新用户不知道怎么开始；破坏性操作没有危险语义 |
| 12 | **Fusion 默认控件混搭** | `main.cpp:48` `QQuickStyle::setStyle("Fusion")`；Settings 用默认 `ComboBox/Button`，其余是自绘 Rectangle | 同一个页面里两种圆角、两种高度、两种 hover |
| 13 | **无动效、无 focus ring** | 只有窗口 dock 的 200ms 动画；所有自定义控件没有键盘焦点视觉 | 键盘用户不知道焦点在哪；状态切换生硬 |

> 一个反例值得保留：`PluginCard.qml` 已经把"固定高度 + 状态驱动视觉 + 只发信号"做对了，新的组件层应该沿用这种写法。

---

## 2. 目标与非目标

**目标**

1. **一眼可读**：标题/正文/说明三级对比明确，所有正文和说明达到 4.5:1。
2. **一套规则**：颜色、间距、圆角、字号只在 `Style.qml` 定义；页面不再出现十六进制色值。
3. **有反馈**：hover / pressed / focus / disabled / loading 五种状态统一。
4. **更轻**：去掉 emoji 结构图标，改单色图标；减少大面积高饱和色块。
5. **可逆**：分阶段落地，每一步都能单独回滚，不改任何业务行为。

**非目标（本轮不做）**

- 不改窗口尺寸（搜索 600×420 / 主面板 760×520），不改热键、不改 dock 行为。
- 不引入真正的背景模糊 / Mica（P3 可选项，见 §7.3）。
- 不重写待办/笔记/剪贴板的交互模型（只换视觉与反馈）。
- 不做移动端/响应式布局。

**性能预算**：列表滚动 60fps；冷启动时间不增加；安装包增量 < 1MB（图标字体）。

---

## 3. 方向选择

| 方向 | 关键词 | 优点 | 代价 | 适合 |
|------|--------|------|------|------|
| **A 柔光玻璃（推荐）** | 深色优先、蓝紫渐变、半透明层叠、细边框、柔影 | 和现有蓝色连续；截图好看；主要改 QML，不动 C++ | 外阴影需要窗口留边或 DWM 处理；深色下阴影要改用"边框 + 顶部高光" | 搜索/启动器类工具 |
| B 极简纸感 | 纯中性色、hairline、几乎无阴影、衬线标题 | 安静、耐看、实现最简单 | 暗色表现弱；对排版和留白要求极高，做不好会"像没设计" | 阅读/笔记为主 |
| C Windows 11 原生 Fluent | Mica/Acrylic、Segoe UI Variable、系统控件 | 最"原生"，和系统设置一致 | vcpkg Qt 下 Mica 要自己调 DWM；Fusion 控件要逐个重绘；工作量最大 | 想完全融入 Windows |

**推荐 A**，理由：

- 现有强调色 `#4a90d9` 本身就是蓝色，改成 `#6E8DFF` 是延续而不是推翻；
- 深色是主场景（README 里的截图和日常使用），A 在深色下收益最大；
- 不需要引入新的渲染技术就能完成 90%；真正的 Mica 留作 P3 可选项。

视觉稿见顶部图片：左边深色、右边浅色；上面主面板、下面搜索浮窗。

---

## 4. 设计语言（Design Tokens）

### 4.1 颜色

原则：**深色下"越靠前越亮"**；浅色下"越靠前越白 + 阴影越明显"；边框只做 1px 分隔，不抢内容。

| Token | 深色 | 浅色 | 用途 |
|-------|------|------|------|
| `bg` | `#171A1F` | `#F7F8FB` | 窗口底 |
| `surface` | `#1D2127` | `#FFFFFF` | 卡片、输入框、列表行 |
| `surface2` | `#242A32` | `#EEF1F6` | hover、次级按钮、kbd |
| `surface3` | `#2C333D` | `#E4E9F1` | pressed、分隔块、tag 灰底 |
| `line` | `rgba(255,255,255,.08)` | `rgba(20,29,45,.08)` | 1px 分隔 |
| `lineStrong` | `rgba(255,255,255,.14)` | `rgba(20,29,45,.14)` | 输入框/浮层边框 |
| `text` | `#E9ECF1` | `#151A22` | 主文字 |
| `text2` | `#AAB3C0` | `#525D6E` | 正文次级 |
| `text3` | `#808A99` | `#6B7585` | 说明、时间、占位（≥4.5:1） |
| `accent` | `#6E8DFF` | `#2F5FE0` | 主按钮、选中、关键数字 |
| `accent2` | `#9B7BFF` | `#6F49E8` | 渐变第二色、少量点缀 |
| `accentSoft` | `rgba(110,141,255,.14)` | `rgba(47,95,224,.10)` | 选中行/激活 chip 底 |
| `success` | `#45C48C` | `#159A63` | 已连接、已保存 |
| `warning` | `#EEB154` | `#B57311` | 待整理、注意 |
| `danger` | `#EF6B6B` | `#D14848` | 删除、失败 |

补充规则：

- `accent` 全屏只出现在 **3 类位置**：主按钮、当前选中、关键数字/状态点。8 个 chip 不要全蓝——未选中用 `text2`，选中用 `accentSoft + accent`。
- 文件类型色只保留 4 组（file/app/doc/calc），用 **淡色底 + 同色图标**，不再用整块高饱和底 + 白字。
- 深色下阴影几乎不可见，靠 `line + inset 顶部高光` 建立层级；浅色下才用阴影。
- 对比度：`text3` 深色对 `bg` ≈ 4.98:1、浅色对白 ≈ 4.65:1；旧的 `#666/#999` 只有 2.9/2.85，必须替换。

### 4.2 间距、圆角、尺寸

4pt 基数，常用 4 / 8 / 12 / 16 / 20 / 24 / 32。

| 项 | 值 | 说明 |
|----|----|------|
| 页面内边距 | 20 | 所有页统一；现在 0/8/10 混用 |
| 卡片内边距 | 14–16 | 列表行 12 |
| 卡片间距 | 8–10 | 列表行间距 4–6 |
| 侧栏宽度 | 158–168 | 当前 150，略窄 |
| 标题栏 | 44 | 主面板自绘时 |
| 行高 | 44 / 49 / 52 / 56 | 导航 / 待办 / 搜索结果 / 设置行 |
| 圆角 | 6 / 8 / 10 / 12 / 16 | 小控件 / 按钮 / 输入 / 卡片 / 窗口 |
| 触控/点击最小边 | 28×28 | 图标按钮；行内操作 24×24 仅桌面 |
| 焦点环 | 2px `accent` 40% + 1px offset | 所有可聚焦控件 |

### 4.3 字体

- 字族：`Segoe UI Variable Text, Segoe UI, Microsoft YaHei UI, Microsoft YaHei, sans-serif`。QML 用 `font.families`（Qt 6）或启动时 `QGuiApplication::setFont` 统一兜底。
- 字号/字重：

| 角色 | 字号 | 字重 | 行高 |
|------|------|------|------|
| 页面标题 | 19–20 | 600 | 1.25 |
| 卡片/区块标题 | 14–15 | 600 | 1.35 |
| 正文 | 12.5–13 | 400/500 | 1.5 |
| 说明/时间 | 10–10.5 | 400 | 1.4 |
| 导航 | 12.5 | 500（激活 600） | 1 |
| 数字/计数 | 继承 | 500 | `tnum` 等宽数字 |

- 中文不要加 letter-spacing；英文/数字可 `-0.01em`。
- 计数、时间、版本号加 `font.features: {"tnum": 1}`（Qt 6.7+；本项目 Qt 6.11 可用），避免数字跳动。
- 不默认打包中文字体：系统字体零成本、授权干净。若追求视觉稿的 MiSans 效果，P3 再做子集（见 §7.3）。

### 4.4 图标

- 结构图标全部改为**单色线性图标**，20×20 网格、1.8px 描边、圆头圆角。
- **实施选择**：最终没有引入 TTF 图标字体，而是用 `QtQuick.Shapes` + `PathSvg` 直接画 24×24
  path（`components/AppIcon.qml`）。好处是零二进制资源、任意 DPI 清晰、`color` 直接染色；
  代价是每加一个图标要补一条 path。图标清单目前覆盖导航、文件类型、操作和状态。
- 备选方案仍是 **Fluent System Icons（MIT）** 图标字体：一个文件、`color` 可染色；如果后续
  图标数量继续增长，再切换到字体更省维护。

### 4.5 阴影与层级

| 层级 | 用法 | 实现 |
|------|------|------|
| e0 | 页面底 | 无阴影 |
| e1 | 卡片/列表行 | 1px `line` + 浅色下 `0 1px 2px rgba(28,38,58,.05)` |
| e2 | 浮窗/下拉 | `MultiEffect` 阴影（仅 1 个实例，不给每行用） |
| e3 | 模态/确认框 | e2 + 遮罩 `rgba(0,0,0,.35)` |

**不要**在 ListView delegate 里用模糊阴影：1000 行就是 1000 次 blur。列表行只用边框和底色。

### 4.6 动效

| 场景 | 时长 | 曲线 |
|------|------|------|
| hover / 颜色 | 90ms | OutCubic |
| 选中 / 展开 | 140ms | OutCubic |
| 页面切换 | 140ms | OutCubic（fade + 4px 位移） |
| 窗口显示/隐藏 | 120–160ms | OutCubic（scale .98 → 1） |
| dock / undock | 180ms | OutCubic（保留现有 200ms 也可） |

不做弹跳、不做无限循环动画。以后加"减少动效"配置时，用一个 `Style.motion` 开关统一控制。

---

## 5. 组件层（新增 `resources/qml/components/`）

目标：**页面里不再出现裸 `Rectangle + MouseArea` 当按钮**。每个组件一个文件，统一状态和尺寸。

| 组件 | 替代 | 关键 API / 状态 |
|------|------|----------------|
| `AppButton` | 各处自绘按钮 | `variant: primary/secondary/ghost/danger`、`size: sm/md`、`loading`、`enabled`、`icon` |
| `AppIconButton` | 🏠 / ✕ / ⋯ | `icon`、`tooltip`、`variant`、`checked` |
| `AppTextField` | 搜索框/待办输入/笔记标题 | `leadingIcon`、`clearable`、`error`、`focusRing` |
| `AppSearchField` | `MainWindow` 搜索条 | 组合 `AppTextField` + 防抖 + IME 保护（逻辑保持原样） |
| `AppChip` | 种类过滤/筛选 | `selected`、`count`、`onToggled` |
| `AppCard` | 各页卡片 | `padding`、`hoverable`、`selected`、`danger` |
| `AppListItem` | 待办/剪贴板/下载行 | `leading`、`title`、`subtitle`、`trailing`、`hoverActions` |
| `AppCheckbox` | 待办勾选 | `checked`、动画勾 |
| `AppSwitch` | 设置开关 | `checked`、`onToggled` |
| `AppComboBox` | 设置页下拉 | 统一高度/圆角/弹层，替换 Fusion 默认 |
| `AppScrollBar` | 所有列表 | 6px、圆角、hover 加宽 |
| `AppSectionHeader` | 页面标题/分组标题 | `title`、`subtitle`、`actions` |
| `AppEmptyState` | 空列表 | `icon`、`title`、`hint`、`actionText` |
| `AppKbd` | 底部快捷键 | `text`、`size` |
| `AppToast` | 清空/复制/归档反馈 | `text`、`kind`、自动消失 |
| `AppWindowFrame` | 主面板标题栏 | 拖动、双击最大化、最小化/关闭、6px 缩放边 |

代码形状（示意，不是最终实现）：

```qml
// components/AppButton.qml
Rectangle {
    id: root
    property string variant: "secondary"   // primary | secondary | ghost | danger
    property bool loading: false
    signal clicked()

    implicitHeight: 34
    radius: Style.radiusControl
    color: !enabled ? Style.surface2
         : pressed ? Style.accentPressed
         : hovered ? Style.accentHover
         : variant === "primary" ? Style.accent
         : variant === "ghost" ? "transparent"
         : Style.surface2
    border.width: variant === "secondary" ? 1 : 0
    border.color: Style.line

    HoverHandler { id: hover }
    TapHandler { onTapped: if (root.enabled) root.clicked() }
    // 键盘：Keys.onSpacePressed / onReturnPressed 走同一个 clicked()
}
```

**约定**

- 组件只负责视觉与交互反馈，不碰业务；点击一律 `signal`。
- 颜色/尺寸只从 `Style` 取，不写十六进制。
- 所有组件设置 `Accessible.role` / `Accessible.name`；自绘按钮必须能用 Tab 聚焦。
- 组件不依赖页面 id；需要主题时读 `Style` 单例。

> 不用 QtQuick Controls 的 `Button` 再套一层皮肤，是因为当前 Fusion 的默认内边距和字体度量会反复打架；自绘 + 统一 token 更可控。`ComboBox` 这类弹层复杂的控件可以保留 Controls，只定制 `background/contentItem/delegate`。

---

## 6. 页面方案

### 6.1 搜索浮窗（最高频，P0 先做）

目标：像"系统级命令面板"，而不是"一个带搜索框的窗口"。

```
┌────────────────────────────────────────────┐ 600×420
│ ⌕  设计▏                            Alt Space│ 56
├────────────────────────────────────────────┤
│ ●全部  文档  图片  视频  音频  压缩  代码  PDF│ 40
├────────────────────────────────────────────┤
│ 最佳匹配 5                                  │
│ [▤] 文件治理功能设计.md                 ↵   │ 52
│ [▣] Visual Studio Code                  APP │
│ [▤] 季度汇报.pptx                      PPTX│
│ [#] 128 × 24 = 3072                    CALC│
│ [▤] 设计资源                         FOLDER│
├────────────────────────────────────────────┤
│ 5 个结果 · 已索引 31,845 个文件    ↑↓ ↵ Esc │ 36
└────────────────────────────────────────────┘
```

具体改动：

1. **窗口**：圆角 14、1px `lineStrong`、顶部 inset 高光；外阴影见 §7.3（P0 可先不加，不影响布局）。
2. **搜索行**：高 56；搜索图标用 `accent`；占位文案轮换（"搜索应用、文件、计算…" / "试试 :doc / in:downloads"）；右侧显示当前热键 `Alt Space` 的 kbd。
3. **Chip 行**：高 40；chip 高 25；选中 = `accentSoft` 底 + `accent` 字 + 30% 边框，未选中 = 透明底 + `text2`。8 个 chip 仍可放下；窗口更窄时横向滚动而不是挤压。
4. **结果行**：52–56；类型图标 30×30 圆角 9 的淡色底；标题 12.5/600，命中字符用 `accentSoft` 底 + `accent` 字；副标题 9.5–10 灰；选中行 `accentSoft` 底 + 左侧 2px accent 条（不再整行实心蓝）；hover 显示 `↵`。
5. **底部**：36；左侧状态用 `tnum`；右侧快捷键改成 kbd 小键帽；`Esc` 永远关闭。
6. **空状态**：`⌕` 大图标 + "没有找到「xxx」" + 两三个建议 chip（"去掉 :doc"、"按 Alt+Space 重新唤起"）。
7. **动效**：显示时 opacity 0→1 + y -4→0（120ms）；查询变化时列表 80ms 淡入，不做逐行 stagger。
8. **Dock**：现在 dock 后只露 8px 纯背景，用户不知道那是什么。建议改成露 36–40px 的"抓手条"：左侧 app 标记 + "⌕ 搜索"，hover 展开；这是交互改动，放 P3，默认不做。

> 明确不做：搜索结果里的模糊命中高亮需要 C++ 返回匹配区间，当前 `SearchResult` 没有这个字段。P0 只高亮"查询串的字面包含"，P2 再评估是否给 `SearchResult` 加 `matchStart/matchLength`。

### 6.2 主面板

```
┌──────────────────────────────────────────────────────┐ 760×520
│ A Assistant │ 主面板                       —  □  ✕  │ 44
├──────────────┬───────────────────────────────────────┤
│ 工作台        │ 待办事项                    ⌕  ⋯     │
│ ✓ 待办事项 ●  │ 3 项进行中 · 今日完成 1 项             │
│ ▤ 记事本      │                                       │
│ ▤ 剪贴板      │ 今天  3 ───────────────────────────── │
│ 效率          │ [ ] 把搜索浮窗改成柔光玻璃质感   设计 14:00│
│ ⬇ 最近下载 2  │ [ ] 给设置页加左侧二级导航       体验 明天│
│ ⟳ 设备同步 ●  │ [ ] 统一 8pt 间距与圆角令牌       规范 周五│
│ ⬡ 插件市场    │                                       │
│              │ [+] 添加待办，Enter 保存          [↑] │
│ ⚙ 设置        │ ✓ 已自动保存 · 12:04                  │
│ v0.4.0  已连接│                                       │
└──────────────┴───────────────────────────────────────┘
```

1. **窗口框架（P1 的硬骨头）**
   - 现状：`MainPanel.qml` 没设 `flags`，是系统原生标题栏；内容却又画 12px 圆角。二选一：
     - **方案 1（推荐）**：`flags: Qt.FramelessWindowHint | Qt.Window`，自绘 44px 标题栏；拖动用 `startSystemMove()`，边缘缩放用 `startSystemResize()`（Qt 6.5+），最小尺寸 720×480。视觉统一，但要做 6px 热区和 Aero Snap 验证。
     - **方案 2（省事）**：保留原生边框，去掉 12px 圆角，`color` 设为 `Style.bgColor`。视觉差一截，但零风险。
   - 视觉稿按方案 1 画。若拍板方案 2，标题栏和窗口圆角相关条目作废，其余不变。
2. **侧栏**：158–168 宽、12 圆角、`surface` 底；分"工作台 / 效率"两组，组标题 9.5px `text3`；导航项 34 高、图标 15、激活 = `accentSoft` + 左侧 3px accent 条 + 600 字重；"最近下载"加计数 badge，"设备同步"加状态点；底部固定设置 + 版本/连接状态。
3. **页面头**：标题 19/600 + 副标题 10.5 + 右侧图标按钮；所有页面统一 20px 内边距、标题到内容 14px。
4. **列表行**：49–52 高、10 圆角、`surface` 底 + 1px 边框；hover 才显示行内操作（`⋯`、`✕`），默认只留时间；删除用 `danger` 色，并配 Toast/确认。
5. **输入区**：40 高、10 圆角；左侧 `+` 或图标，右侧主按钮 40×40；底部"已自动保存 · HH:mm"用 `success` 小勾。
6. **滚动条**：统一 6px、圆角、hover 8px，颜色 `surface3`，不要系统默认宽条。

### 6.3 各页要点

| 页面 | 主要改动 |
|------|----------|
| **待办** | 勾选框 18→20、勾选动画 120ms；完成后降为 `text3` + 删除线并沉底；行内操作 hover 才出现；空状态给"添加第一条"按钮；顶部加"今天/全部"轻量分段（可选，先只做视觉） |
| **记事本** | 左列表 220→240，选中 `accentSoft` 而不是实心蓝；标题输入 18/600、正文 14/1.7；保存状态放 footer（"已保存 · 刚刚"）；删除移到 `⋯` 菜单 + 危险色，不再常显灰色按钮 |
| **剪贴板** | 行 hover 不再整行铺 accent，只 `surface2`；内容 2 行截断；右侧相对时间 + 悬停原文；顶部加搜索框；"清空"改为 `danger` ghost + 确认 Toast（可撤销） |
| **最近下载** | 文件类型块 34→36，淡色底 + 类型字；状态点替代整圈描边；行高 70→64；筛选 chip 用 `accentSoft` 选中；`⋯` 菜单统一；空状态区分"待整理为空"和"还没有下载" |
| **设备同步** | 879 行、信息密度最高：首屏只留"状态 + 主操作"，把长说明折叠进"如何工作"；配对码用 6 位等宽大字 + 复制按钮；设备列表用头像/首字母 + 状态点；危险操作（解除配对）放页尾 danger 区；进度条统一 6px 圆角 |
| **插件市场** | 卡片 64 保持（`PluginCard` 约束不能破）；图标容器 32 圆角 8；状态 badge 统一；安装进度条 3px；搜索框 + 分类 chip；加载态用 3 行 skeleton，不要空白 |
| **设置** | 814 行单列 → 左侧二级导航（通用 / 外观 / 文件索引 / 收件箱 / 日志 / 关于），右侧内容区；分组标题 12/600；每行 48 高，标签左、控件右；危险区（清空数据）单独一屏；保留 Flickable 但只滚右侧 |

---

## 7. 工程落地

### 7.1 文件与资源

```
resources/qml/
├── Style.qml                 # 扩成完整 token 表（仍叫 Style，避免 8 个文件改 import）
├── components/
│   ├── qmldir
│   ├── AppButton.qml
│   ├── AppIconButton.qml
│   ├── AppTextField.qml
│   ├── AppChip.qml
│   ├── AppCard.qml
│   ├── AppListItem.qml
│   ├── AppCheckbox.qml
│   ├── AppSwitch.qml
│   ├── AppScrollBar.qml
│   ├── AppSectionHeader.qml
│   ├── AppEmptyState.qml
│   ├── AppKbd.qml
│   ├── AppToast.qml
│   └── AppWindowFrame.qml
├── icons/                    # 若用 SVG 方案
└── fonts/                    # 若用图标字体
```

- **`Style.qml` 不改名**：它已经被 6 个页面和两个 engine 使用，改名只会制造 8 处无意义 diff。内部扩展 token，旧字段保留别名过渡：

```qml
// 旧名保留，页面可以逐个迁移
readonly property color bgColor: bg
readonly property color panelColor: surface
readonly property color mutedColor: text3
```

- **`resources.qrc` 必须手工加文件**：当前是显式列表（`resources.qrc:3-15`），新增组件/图标/字体都要加 `<file>`。
- **`CMakeLists.txt` 的 `QRC_RESOURCE_FILES` glob 只覆盖 `resources/qml/*.qml`、`qmldir`、`market/*.json`**；新增 `icons/*.svg`、`fonts/*.ttf` 要么加 glob 模式，要么接受"改图标不触发 rcc"的坑。新增/删除 QML 文件后仍需重新 configure（CLAUDE.md 的 GLOB 约定）。
- **两个 engine**：搜索浮窗和主面板各有一个 `QQmlApplicationEngine`，`Style` 单例在每个 engine 里各实例化一份；token 只读，不存在同步问题。

### 7.2 迁移顺序（避免大爆炸）

1. 先在 `Style.qml` **加**新 token，不删旧名；所有页面仍能跑。
2. 新建 `components/`，先做 `AppButton/AppIconButton/AppKbd/AppCard/AppChip` 五个。
3. 用搜索浮窗做第一个试点（文件最少、收益最大），验证 token + 组件 + 截图流程。
4. 主面板框架换皮（标题栏/侧栏/页头），再把 Todo/Notes/Clip 三个简单页迁到组件。
5. 迁移 Downloads/Sync/Plugins/Settings（复杂页），最后删旧 token 别名、清硬编码色。
6. 每一步都可单独提交、单独回滚；P0 结束前不动业务页的 C++。

### 7.3 关键技术点与坑

| 点 | 方案 | 备注 |
|----|------|------|
| **窗口外阴影** | QML 画不出窗口外。两条路：① 窗口四周留 20–24px 透明边，内容内缩，阴影用 `MultiEffect`；② C++ 给 frameless 窗口加 `WS_THICKFRAME` + `DwmExtendFrameIntoClientArea`，用 DWM 原生阴影 | ① 要改窗口尺寸、保存位置、dock 计算，P0 不做；P0 用 1px 边框 + 顶部高光近似，P2 再评估。视觉稿画的是①的效果 |
| **真正的 Mica/Acrylic** | `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)`（Win11 22H2+）或 `SetWindowCompositionAttribute`（Win10，非公开 API） | 可选 P3；Win10 不支持 Mica；开透明后 QML 合成开销和文字可读性都要复测。方向 A 不依赖它 |
| **Frameless 拖动/缩放** | `Window.startSystemMove()` / `startSystemResize(edges)`（Qt 6.5+）；边缘 6px 热区；双击标题栏最大化 | 必须测 Aero Snap、最小尺寸、多显示器 DPI 切换 |
| **图标字体** | `QFontDatabase::addApplicationFont(":/fonts/...ttf")` 或 QML `FontLoader`；`Text { font.family: "FluentSystemIcons"; text: "\uE..."; color: ... }` | 比每个图标一个 `MultiEffect` 染色便宜；需要维护 name→codepoint 表；字体授权要随包附 LICENSE |
| **中文字体子集** | MiSans/HarmonyOS Sans 子集化（常用 3500 字 + Latin），每字重 1–1.5MB | 可选 P3；不打包就用系统 `Microsoft YaHei UI`，视觉稿的 MiSans 观感会略不同 |
| **tnum** | `Text { font.features: {"tnum": 1} }` | Qt 6.7+；本项目 6.11 可用。用于计数、时间、版本、配对码 |
| **现有 dock** | `PropertyAnimation` 直接改 `window.y` | 换皮时保留；不要改成 `Behavior on y`，会和手动拖动打架 |
| **QML 控件字体** | 启动时 `QGuiApplication::setFont(QFont("Segoe UI Variable Text", 10))`，再让中文 fallback | 比在每个 `Text` 写 `font.family` 稳；Fusion 控件也会跟随 |
| **插件页** | 插件 QML 在独立 context，无法强制套我们的组件 | 只保证窗口 palette 继承；插件页视觉不统一是已知限制，写进文档 |

### 7.4 性能与可访问性

- **列表**：delegate 固定高度、`clip: true`、避免在 delegate 内用 `MultiEffect`/`DropShadow`；hover 用 `HoverHandler` 而不是整行 `MouseArea`（减少事件层）。
- **绑定**：不在 `width/height` 绑定里做字符串拼接或 `indexOf`；导航 `indexOfKey` 只在 key 变化时算。
- **图标**：图标字体一个 `Text`，比 `Image + MultiEffect` 轻；SVG 方案只在图标数量少时用。
- **可访问性**：自绘控件补 `Accessible.role`、`Accessible.name`、`Accessible.focusable`；焦点环不能只靠颜色，要有 2px 外扩；`Esc`/`Enter`/`Tab` 行为逐页走查。
- **对比度**：用脚本/浏览器 devtools 抽查 `text3`、`accent` 在 `surface/surface2/accentSoft` 上的组合，目标 ≥4.5:1（大字 ≥3:1）。

---

## 8. 里程碑与工时

| 阶段 | 内容 | 产出 | 验收 | 预估 |
|------|------|------|------|------|
| **P0 令牌 + 搜索浮窗** | `Style.qml` 全量 token；`AppButton/AppIconButton/AppKbd/AppCard/AppChip`；搜索浮窗换皮；行选中/悬停/空状态 | 可运行的搜索浮窗新版；组件目录 | 深/浅截图；键盘走查；无硬编码色 | 0.5–1 人日 |
| **P1 主面板框架** | 自绘标题栏（或方案 2）；侧栏分组；页头统一；Todo/Notes/Clip 迁组件；滚动条 | 主面板新版框架 + 3 页 | 760×520 不溢出；拖动/最小化/关闭正常；键盘可达 | 1–1.5 人日 |
| **P2 复杂页 + 图标** | Downloads/Sync/Plugins/Settings；设置二级导航；图标字体替换 emoji；危险操作 + Toast | 全部 8 页换皮 | 每页 light/dark；1000 行列表 60fps；插件页不回归 | 2 人日 |
| **P3 打磨（可选）** | 真外阴影/Mica、dock 抓手条、字体子集、托盘菜单、动效细化 | 细节版 | 录屏；DPI 100/125/150%；冷启动无变化 | 1 人日 |

合计 **4.5–5.5 人日**（晚上做大约一周半）。建议顺序：P0 → 拿给用户看 → P1 → P2；P3 按需。

**如果只想先看效果**：只做 P0（搜索浮窗）就能覆盖 80% 的"变好看了"感知，风险也最小。

---

## 9. 验收清单

- [ ] `resources/qml` 下除 `Style.qml` 外无 `#[0-9a-fA-F]{6}` 硬编码色（`grep` 验证）。
- [ ] 所有可点击元素有 hover / pressed / focus；Tab 能走完搜索浮窗和主面板。
- [ ] 正文/说明对比度 ≥ 4.5:1；`text3` 在 `bg/surface/surface2` 上都达标。
- [ ] 深/浅两套在 100% / 125% / 150% DPI 下无错位、无裁切、无 1px 模糊线。
- [ ] 600×420 与 760×520 下不出现意外滚动条（设置页允许纵向滚动）。
- [ ] 1000 条列表快速滚动无明显掉帧；delegate 内无 blur。
- [ ] 结构图标无 emoji（插件图标除外）；图标基线对齐。
- [ ] 关闭/最小化/拖动/热键/`Esc`/双击标题栏行为与现在一致。
- [ ] 插件页、同步页、下载页的功能路径不回归；`ctest` 16 项通过。
- [ ] 安装包增量 < 1MB（图标字体）；冷启动时间不增加。
- [ ] 截图归档：4 个主页面 × 2 主题，存 `docs/features/ui-preview/`。

---

## 10. 风险与取舍

| 风险 | 影响 | 应对 |
|------|------|------|
| Frameless 主面板的缩放/Aero Snap 在 Windows 上边界情况多 | 窗口拖不动、贴边异常 | P1 先做方案 2（原生框）也可；或只做标题栏自绘、保留系统缩放（`WS_THICKFRAME`） |
| 外阴影要改窗口尺寸 | 保存位置、dock、居中逻辑全要跟着改 | P0 不做；用边框 + 高光；P2 单独评估 |
| 图标字体 codepoint 映射维护 | 图标显示成方框 | 用官方 TTF + 一张映射表 + 一个 `AppIcon` 兜底；LICENSE 随包 |
| 中文字体子集体积/授权 | 包体变大、授权不清 | 默认系统字体；子集只做可选 P3 |
| QML Controls 与自绘组件混用 | 高度/圆角/字体不一致 | 复杂控件（ComboBox）定制 background/contentItem；简单控件一律自绘 |
| 改 token 名波及 8 个文件 | 大面积编译期/运行期错误 | 旧名保留 alias，逐页迁移，最后删 |
| 插件页无法统一 | 视觉断层 | 文档标注；插件卡片本身已统一，插件内页不承诺 |
| 截图验收靠人工 | 回归发现晚 | 固定 8 张基线图 + 每次 P 阶段对比；可选加 `qmltestrunner` 烟雾测试 |

---

## 11. 需要拍板

1. **方向**：A 柔光玻璃（推荐）/ B 极简纸感 / C Windows 11 原生？
2. **主面板窗口**：接受 frameless 自绘标题栏（含缩放边缘处理），还是先保留原生边框（方案 2）？
3. **图标**：引入 Fluent System Icons 图标字体（约 300KB）替换 emoji，还是继续用 emoji 只调大小/颜色？
4. **字体**：只用系统字体（0 成本，Win10/11 略有差异），还是 P3 打包 MiSans 子集（+1.5–3MB）？
5. **优先级**：先做搜索浮窗（推荐，收益最快），还是先做主面板？
6. **动效**：是否接受窗口显示/页面切换加 120–140ms 动效；是否需要"减少动效"开关？
7. **待办交互**：保持现在的"记录 → 勾选划去 → 沉底"，还是借这次改版加"今天/全部"分组？

拍板后按 §8 的 P0 开始，第一步只改 `Style.qml` + 搜索浮窗 + 5 个基础组件，不动业务代码。

---

## 附录 A：旧 token → 新 token 对照

| 旧 | 新 | 说明 |
|----|----|------|
| `bgColor` `#2c2c2c` / `#f0f0f0` | `bg` `#171A1F` / `#F7F8FB` | 深色压暗、浅色提亮 |
| `panelColor` `#1e1e1e` / `#ffffff` | `surface` `#1D2127` / `#FFFFFF` | 深色卡片改为比背景亮 |
| `textColor` `#e0e0e0` / `#1a1a1a` | `text` `#E9ECF1` / `#151A22` | 略提高对比 |
| `mutedColor` `#666` / `#999` | `text3` `#808A99` / `#6B7585` | 2.9:1 → 4.98:1 / 4.65:1 |
| `accentColor` `#3a6ea5` / `#4a90d9` | `accent` `#6E8DFF` / `#2F5FE0` | 深色更亮、浅色更深，保证对比 |
| `activeBorder` `#5a8ec9` / `#4a90d9` | `accentBorder` | 只用于选中/焦点边 |
| `borderColor` `#3d3d3d` / `#d0d0d0` | `line` / `lineStrong` | 拆成两级 |
| `controlColor` `#3a3a3a` / `#e8e8e8` | `surface2` | 统一控件底 |
| `radius 8 / radiusSmall 6` | `radius8 / radius6 / radius10 / radius12 / radius16` | 按控件层级 |
| `titleSize 18 / bodySize 13 / captionSize 11` | `fontTitle 19 / fontBody 13 / fontCaption 11 / fontMicro 10` | 增加 15/12/10 档 |

## 附录 B：参考

- Windows 11 Fluent Design：层级、Mica、圆角与焦点规则。
- Raycast / Spotlight：搜索浮窗的"单行输入 + 分组结果 + 底部键位"结构。
- Fluent System Icons（MIT）/ Lucide（ISC）：单色线性图标。
- WCAG 2.1：正文 4.5:1、大字 3:1、焦点可见。
