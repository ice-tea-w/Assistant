# CLAUDE.md — Assistant 桌面助手项目

## 项目概要

Windows 桌面助手软件，C++20 + Qt6 Quick (QML) + CMake + vcpkg。学习项目，边做边学。

## 技术栈速查

| 领域 | 选型 |
|------|------|
| 编译器 | VS2022 MSVC (`-G "Visual Studio 17 2022" -A x64`) |
| 构建 | CMake 3.20+, `CMAKE_CXX_STANDARD 20` |
| 包管理 | vcpkg (`C:\vcpkg`, `x64-windows` triplet) |
| GUI | Qt 6 Quick (QML) + QuickControls2 |
| JSON | nlohmann_json (vcpkg) |
| 日志 | spdlog (vcpkg, `SPDLOG_FMT_EXTERNAL`) |
| 数据库 | Qt6::Sql (内置 SQLite 驱动, WAL 模式) |
| 配置路径 | `%LOCALAPPDATA%/AssistantTeam/Assistant/` |

## 编译命令

```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Debug
```

新增/删除源文件后必须重新 configure，因为用了 `file(GLOB_RECURSE)`。

> **环境注意（本机 sandbox）**：编译必须用 `danger-full-access`。受限模式下 CMake/AUTOMOC 调 `moc.exe` 子进程会被拦（`libuv process spawn failed: operation not permitted`），`workspace-write` 不够。

## 运行方式

```powershell
.\build\Debug\run.bat
```

`run.bat` 由 CMake POST_BUILD 自动生成，负责：
- 把 vcpkg Qt6 `bin/` 加入 PATH（Windows DLL 加载器需要）
- 在 exe 目录启动程序

> **环境注意（本机 sandbox）**：**运行也需要 `danger-full-access`**。应用要写 `%LOCALAPPDATA%/AssistantTeam/Assistant/`（日志 / 配置 / 4 个 DB），`workspace-write` 下写入被拒 → `Logger::init` 失败退回 console-only，**日志不落盘**，容易误判成"启动正常"。
> 冒烟判断启动是否成功时**不要看 stdout**：spdlog 的 console sink 在重定向后是全缓冲的，进程被强杀会丢输出；看 `%LOCALAPPDATA%\AssistantTeam\Assistant\logs\assistant.log` 才可靠。

> **文件索引引擎开关（A2 起）**：`config.json` 的 `fileIndex.engine` —— `"v2"`（默认，`FileIndex` 门面 + 后台扫描）
> 或 `"legacy"`（旧 `FileSearcher` 同步建索引，A1 行为，A7 之前一直保留）。子进程驱动应用做验收时用
> `_probe/hotkey_probe.ps1`（`PostMessage(WM_HOTKEY)` 走真实消息链，见 `docs/devlog/2026-10-16.md`）。

## Qt 部署原理（vcpkg 开发阶段）

两层机制：
1. **qt.conf**（exe 同级，POST_BUILD 生成）：Qt 内部查找 plugins/qml → vcpkg `Qt6/plugins` `Qt6/qml`
2. **run.bat PATH**：Windows `LoadLibrary` 找 QML 插件依赖的 `Qt6*` DLL

`windeployqt` 在 vcpkg 版 Qt 不可用。

## 当前开发阶段

**Phase 3 收尾中**（扩展功能）

已完成：
- Phase 0: Hello 窗口、编译环境
- Phase 1 MVP: 系统托盘 + 全局热键 (Alt+Space) + QML 窗口 + 配置读写 + 主题
- Phase 2 (v0.2.0): 日志系统 / 搜索历史 SQLite / FuzzyMatcher 五级打分 / Calculator / AppIndexer 应用启动器 / FileSearcher 文件搜索 / QML 适配 / 集成测试
- Phase 3 主面板: 3.0 主面板骨架（左侧导航 + StackLayout 四页，与搜索浮窗并存）
- Phase 3 待办: `TodoDatabase` + `TodoPage`（记录 → 完成划去）
- Phase 3 记事本: `NotesDatabase` + `NotesPage`（CRUD + 编辑）
- Phase 3 剪贴板: `ClipboardDatabase` + `ClipboardPage` + `startClipboardMonitor`
- Phase 3 设置页: 主题切换 / 日志级别 / 当前热键显示

Phase 3 剩余（收尾中，写在这里便于续接）：
- [x] 热键自定义（设置页捕获组合键 → `PanelBridge::applyHotkey` → `HotkeyManager::reRegister`，失败自动回滚原热键）
- [x] 开机启动（`core/AutoStart` 写 `HKCU\...\Run`，注册表是唯一真相来源，不写进 config.json）
- [x] 剪贴板相对时间显示（`createdAtEpoch` + QML 相对时间函数 + 每分钟自刷新 + 悬停显示原始时间）
- [x] QML 主题色抽 `Style.qml` 单例（`resources/qml/qmldir` 声明 singleton，6 个页面的重复色值收敛为一处）
- [ ] `v0.3.0` 里程碑：**真机手动走查四页交互**（版本号 bump 已完成 → `0.3.0`，QML 显示走 `appVersion`）
- [ ] （可选，GUIDE 3.5）崩溃报告 Crashpad

**v0.4.0 文件子系统 Phase A**（`docs/architecture/v0.4-实施清单.md`）：
- [x] **A1 止血**（2026-09-24，独立 patch）：P0-1 不跟随 junction（`NoSymLinks` 实测无效 → 自控递归 + `isJunction()`）、
      P0-2 目录级剪枝、P1-5 历史写入挪到执行路径、P1-6 QML 防抖 + IME 拦截；三条验收全过。
      详见 `docs/devlog/2026-09-24.md`
- [x] **A2 结构**（2026-10-16）：`FileIndex` 门面 + `FileSnapshot`（不可变只读快照）+ `SnapshotManager`（COW 发布，
      `std::atomic<std::shared_ptr<const T>>`）+ 后台扫描线程；`fileIndex.engine = "v2"`（默认）| `"legacy"`（一行可退）。
      三条验收全过（真机冷启动 + 20 万条 golden 132 项 0 失败）。详见 `docs/devlog/2026-10-16.md`
- [ ] A3 采集（Win32 扫描 + `FileIndexDatabase` + `DatabaseMigrator` + 差量）… A8 拼音首字母

> ✅ **P0-3 已在 v2 路径修掉**：不再套 2 万条任意截断，真机索引 20000 → **31810** 条，
> `Documents`/`Downloads` 终于进得来；而每次按键的全表扫描 **333 ms → 326 ms（没有变慢）**。
> ⚠️ 但**当前打分算法本身就慢**：真机 31810 条单遍全表扫描 **326 ms**、合成 20 万条 **13.2 s** ——
> 架构文档里"现状 2 万条 5–15 ms"的估计**低了 20 倍以上**（`FuzzyMatcher::score()` 每次调用两次 `toLower()` + 词边界分支的 `t.mid(i)` 分配）。
> 这是 **A4 baseline 要解决的第一件事**。

> 已知限制：热键捕获时，**当前已注册的全局热键**（如 Alt+Space）会被系统先消费，QML 收不到该按键。
> 想重新绑定成同一个组合键无效，绑定成别的组合键正常。

## 版本与阶段规划（**两套 Phase 编号不要混**）

| 版本 | 内容 | 出处 |
|---|---|---|
| `v0.3.0` | Phase 3 扩展功能（主面板 + 待办/笔记/剪贴板/设置） | `GUIDE.md` Phase 3 |
| `v0.4.0` | **文件子系统 Phase A：搜索基建** | `docs/architecture/文件检索重构方案.md` + `v0.4-实施清单.md` |
| `v0.4.x` | 文件治理 Phase B：种类过滤 + 工程排除 + 查询语法 | `docs/architecture/文件治理功能设计.md` §1 |
| `v0.4.x–v0.5.x` | 文件治理 Phase C/D/E：Download Box / 去向追踪 / 自动治理与多源 | 同上 §2–§3 |
| `v0.6.0` | GUIDE Phase 4：插件系统 + 发布（原定 v0.5.0，因插入文件子系统而顺延） | `GUIDE.md` Phase 4 |

> `GUIDE.md` 的 **Phase 0–4 是项目阶段**；架构文档里的 **Phase A–E 是文件子系统内部阶段**，两者不是一套编号。
> 落地顺序：Phase 3 收尾 → 文件子系统 A（搜索基建）→ B → C/D/E → 插件系统 + 发布。**插件系统必须排在 A 之后**（GUIDE 自己要求"核心架构稳定后再做"，A 阶段的 `ISearchProvider` 就是那个稳定点）。

## 核心模块

| 模块 | 文件 | 职责 |
|------|------|------|
| Logger | `include/core/Logger.h`, `src/core/Logger.cpp` | 命名空间级单例, 双 sink (控制台+文件滚动 5MB×3) |
| ConfigManager | `include/core/ConfigManager.h`, `src/core/ConfigManager.cpp` | JSON 配置读写, Q_PROPERTY 暴露给 QML |
| HistoryDatabase | `include/database/HistoryDatabase.h`, `src/database/HistoryDatabase.cpp` | SQLite 3 张表 (search/command/app_launch history) |
| AppBridge | `include/ui/AppBridge.h`, `src/ui/AppBridge.cpp` | QML↔C++ 桥梁, Q_PROPERTY + Q_INVOKABLE |
| SearchResult | `include/ui/SearchResult.h` | 统一搜索模型 (id, title, subtitle, type, priority, score, resultText, path) |
| FuzzyMatcher | `include/core/FuzzyMatcher.h`, `src/core/FuzzyMatcher.cpp` | 五级模糊匹配打分 (精确>前缀>词边界>包含>编辑距离) |
| AppIndexer | `include/core/AppIndexer.h`, `src/core/AppIndexer.cpp` | 扫描 Start Menu `.lnk` (IShellLink COM), 解析目标路径, 去重排序 |
| FileSearcher | `include/core/FileSearcher.h`, `src/core/FileSearcher.cpp` | 自控前序 DFS（`walkDir`，不跟随 junction、目录级剪枝）+ FuzzyMatcher 打分 top-N, 索引上限 2 万。**A2 起是 `engine=legacy` 的退路**，A7 删除 |
| FileSnapshot | `include/core/FileSnapshot.h` | 不可变只读快照（`Builder` 写入 → `nameAt/lowerAt/pathAt/sizeAt/mtimeAt` 读出）；物理表示藏在这层之后，查询层不摸容器 |
| SnapshotManager | `include/core/SnapshotManager.h`, `src/core/SnapshotManager.cpp` | 快照的 COW 发布（`std::atomic<std::shared_ptr<const T>>`）；读侧取一份即用，无需加锁 |
| FileIndex | `include/core/FileIndex.h`, `src/core/FileIndex.cpp` | 文件索引**门面**（薄）：`startAsync` 后台扫描 + `query` + `stats`；`engine=v2` 时经 AppBridge 提供文件结果 |
| Calculator | `include/core/Calculator.h`, `src/core/Calculator.cpp` | 递归下降解析器, PEMDAS + 一元正负号 + 括号 |
| TrayManager | `include/ui/TrayManager.h`, `src/ui/TrayManager.cpp` | 系统托盘图标 + 右键菜单 |
| HotkeyManager | `include/core/HotkeyManager.h`, `src/core/HotkeyManager.cpp` | Windows RegisterHotKey 全局热键（可重注册） |
| PanelBridge | `include/ui/PanelBridge.h`, `src/ui/PanelBridge.cpp` | 主面板 QML↔C++ 桥梁（待办/笔记/剪贴板列表 + CRUD + 剪贴板监听） |
| TodoDatabase | `include/database/TodoDatabase.h`, `src/database/TodoDatabase.cpp` | SQLite 待办（独立连接 `todo`，`todo.db`） |
| NotesDatabase | `include/database/NotesDatabase.h`, `src/database/NotesDatabase.cpp` | SQLite 笔记（独立连接 `notes`，`notes.db`） |
| ClipboardDatabase | `include/database/ClipboardDatabase.h`, `src/database/ClipboardDatabase.cpp` | SQLite 剪贴板历史（独立连接 `clipboard`，`clipboard.db`，同内容去重） |

> **DB 约定**：每个功能一个独立 DB 文件 + 独立 `QSqlDatabase` 连接名（见 `ClipboardDatabase`），不要共用一个连接。
> **QML 约定**：搜索浮窗走 `AppBridge`，主面板走 `PanelBridge`，两个窗口各管自己的桥。

## Git 规范

- Conventional Commits: `feat(scope):`, `fix(scope):`, `docs:`, `chore:` 等
- Trunk-Based Flow: `main` 上开 feature 分支 → PR → merge

## 关键踩坑记录

详见 `docs/troubleshooting/` 目录：
- `cmake-glob-recurse-新文件不编译.md`
- `qt-automoc-未生成moc文件.md`
- `msvc-中文utf8编码错误.md`
- `qt-headers-大小写错误.md`
- `vs2022-build-tools-安装.md`
- `qt-nosymlinks-挡不住junction.md`（`QDir::NoSymLinks` 挡不住 Windows junction；修复点选错层次会安静地什么都不做）

## 文档结构

- `GUIDE.md` — 完整开发流程指南（长期维护）
- `docs/architecture/` — 架构设计（**文件检索重构方案 / 文件治理功能设计 / v0.4-实施清单**）
- `docs/devlog/` — 开发日志（按日期）
- `docs/features/` — 功能设计 idea
- `docs/learning/` — 学习笔记
- `docs/troubleshooting/` — 踩坑记录
- `.obsidian/` — Obsidian 笔记配置
