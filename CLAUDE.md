# CLAUDE.md — Assistant 桌面助手项目

## 项目概要

Windows 桌面助手软件，C++20 + Qt6 Quick (QML) + CMake + vcpkg。学习项目，边做边学。

## 技术栈速查

| 领域 | 选型 |
|------|------|
| 编译器 | MSVC。生成器名随 VS 版本变：**本机实测可用 `Visual Studio 18 2026`**，装了 VS2022 的机器用 `Visual Studio 17 2022` |
| 构建 | CMake 3.20+, `CMAKE_CXX_STANDARD 20`。**本机 PATH 里没有 cmake**，用 `D:\vcpkg\downloads\tools\cmake-4.4.3-windows-x86_64\bin\cmake.exe` |
| 包管理 | vcpkg（**本机装在 `D:\vcpkg`**，`x64-windows` triplet） |
| GUI | Qt 6 Quick (QML) + QuickControls2 |
| JSON | nlohmann_json (vcpkg) |
| 日志 | spdlog (vcpkg, `SPDLOG_FMT_EXTERNAL`) |
| 数据库 | Qt6::Sql (内置 SQLite 驱动, WAL 模式) |
| 配置路径 | `%LOCALAPPDATA%/AssistantTeam/Assistant/` |

## 编译命令

本机没有 vcpkg 的 `C:\vcpkg`，也没有 PATH 里的 cmake，所以实际命令是：

```powershell
$cmake = "D:\vcpkg\downloads\tools\cmake-4.4.3-windows-x86_64\bin\cmake.exe"
& $cmake -B build -S . -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
    -DVCPKG_TARGET_TRIPLET=x64-windows
& $cmake --build build --config Debug
```

装了 VS2022 与 `C:\vcpkg` 的机器按老写法（`Visual Studio 17 2022` / `C:/vcpkg`）即可，两者是可替代的。

新增/删除源文件后必须重新 configure，因为用了 `file(GLOB_RECURSE)`。

> `CMakeLists.txt` 里 qt.conf 与 run.bat 的 Qt 前缀已改成 `${VCPKG_INSTALLED_DIR}/x64-windows[/debug]`，
> **不再硬编码 vcpkg 路径**；缺失 `VCPKG_INSTALLED_DIR` 会直接 FATAL_ERROR 提示带 toolchain 配置。
> Debug/Release 都能用，构建 Release 时 qt.conf 自动指向非 debug 目录。

## 测试

```powershell
.\build\Debug\assistant_tests.exe                 # 全部用例
.\build\Debug\assistant_tests.exe -o result.txt,txt   # 输出到文件（终端中文会乱码）
```

用 **QTest**（`Qt6::Test`），不是 GTest。新增测试文件要重新 configure。

> 编译前先 `Stop-Process Assistant -Force`：exe 正在运行会报 `LNK1168: 无法打开 Assistant.exe 进行写入`。

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

**设备同步（联网功能）· 已打通第一条完整链路**（`docs/architecture/设备同步功能-设计文档.md`）：
- [x] 引擎 + 共享目录通道 + `sync.db` + `PathGuard` + 冲突副本（2026-10-15，独立 feature 分支）
- [x] 主面板第五页「设备同步」+ `SyncBridge`（工作线程跑引擎，独立 DB 连接）
- [x] **43 个 QTest 用例全绿** —— 本项目第一套自动化测试（此前零测试）
- [x] 界面端到端实测：首次同步上传 1 → 再同步 0 → 改文件后再传 1 → 再同步 0
- [x] 自动同步定时器真机验证：间隔 1 分钟，两实例自动跑出 `上传=1/下载=1`，第二轮 `0/0`
- [x] 单机演示双设备：`Assistant.exe --instance B` 分数据目录（否则两进程共用同一 config，
      压根不是"两台设备"）；全局热键独占，第二个实例靠**托盘菜单新增的「主面板」**才能进同步页
- [x] 设计文档 §4.5 已按实现改成单一 `.sync/manifest.json`（含偏差理由与代价）
- [x] **多本地文件夹**（2026-10-22，FR2）：`SyncRoot{localPath, alias}`，共享目录里按
      `<别名>/<相对路径>` 落盘，下载时剥掉别名。**两台机器对同一逻辑文件夹必须用同一别名**，
      否则各占一棵子树、永远碰不上。本机未参与的子目录静默略过、不计入失败。
- [x] `sync.db` 安全阀：共享目录被清空时会一直跳过（防误删本地文件）—— 补救办法是删
      `%LOCALAPPDATA%\AssistantTeam\Assistant\sync.db`。同步结果现在也落日志（原来只有界面一行状态）
- [x] **快捷同步与投递区**（2026-10-27）：拖拽文件到同步页 → 落到共享目录 `_投递/` →
      对端每 3 秒轮询领取到收件箱 + 托盘通知；文件变化即同步（watcher + 2 秒去抖 + 定时兜底）；
      同步锁 `.sync/lock.json` 串行化多实例写清单；共享目录支持 UNC + 「检查共享」按钮；
      设计见 `docs/architecture/快捷同步与投递区-设计文档.md`。**51 个 QTest 全绿**，
      QML 冒烟无错误；真机双实例拖拽 / UNC 走查待做
- [ ] 冲突列表 UI 没有真机点过（引擎侧与冲突副本文件均已实测）
- [ ] `SyncPage.qml` 多文件夹版界面没有真机点过（QML 无警告，仅代码审查）
- [ ] 直连通道（`LanTransport` + 帧协议）—— 设计文档里的**加分项，没写**

> 核心决策：**把"搬运"外包给网盘**（OneDrive / 坚果云 / Syncthing），程序只做三方差集判断
> （本地现状 / 上次基线 / 对端清单 → 唯一动作）。**新增第三方依赖 0**（Qt6Network 早已链接）。
> 异地直连要 NAT 穿透 + 中继服务器，成本和运维不可控，所以不做。

**v0.4.0 文件子系统 Phase A**（`docs/architecture/v0.4-实施清单.md`）：
- [x] **A1 止血**（2026-09-24，独立 patch）：P0-1 不跟随 junction（`NoSymLinks` 实测无效 → 自控递归 + `isJunction()`）、
      P0-2 目录级剪枝、P1-5 历史写入挪到执行路径、P1-6 QML 防抖 + IME 拦截；三条验收全过。
      详见 `docs/devlog/2026-09-24.md`
- [ ] A2 结构（门面 + COW 快照 + 后台扫描）… A8 拼音首字母

> **P0-3 的"本机撞上限"结论已撤回**（2026-10-15）。2026-09-24 记的 "Desktop 3.5 万文件吃满 20000 上限、
> Documents/Downloads 一个都搜不到" **在本机不复现**：35607 是 A1 之前 junction 循环灌出的虚高值
> （实测曾有 126 条 63 层 `loop/loop/…/real/`）。A1 剪枝后索引为 **9349**，六个根目录
> （Desktop/Documents/Downloads/Pictures/Music/Movies）全部走完，日志里没有 `index capped at 20000`。
> 2 万上限本身仍是待解决的设计问题（按遍历顺序任意截断），但**别拿修复前的 devlog 数据当当前事实**。

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
| FileSearcher | `include/core/FileSearcher.h`, `src/core/FileSearcher.cpp` | 自控前序 DFS（`walkDir`，不跟随 junction、目录级剪枝）+ FuzzyMatcher 打分 top-N, 索引上限 2 万 |
| Calculator | `include/core/Calculator.h`, `src/core/Calculator.cpp` | 递归下降解析器, PEMDAS + 一元正负号 + 括号 |
| TrayManager | `include/ui/TrayManager.h`, `src/ui/TrayManager.cpp` | 系统托盘图标 + 右键菜单 |
| HotkeyManager | `include/core/HotkeyManager.h`, `src/core/HotkeyManager.cpp` | Windows RegisterHotKey 全局热键（可重注册） |
| PanelBridge | `include/ui/PanelBridge.h`, `src/ui/PanelBridge.cpp` | 主面板 QML↔C++ 桥梁（待办/笔记/剪贴板列表 + CRUD + 剪贴板监听） |
| TodoDatabase | `include/database/TodoDatabase.h`, `src/database/TodoDatabase.cpp` | SQLite 待办（独立连接 `todo`，`todo.db`） |
| NotesDatabase | `include/database/NotesDatabase.h`, `src/database/NotesDatabase.cpp` | SQLite 笔记（独立连接 `notes`，`notes.db`） |
| ClipboardDatabase | `include/database/ClipboardDatabase.h`, `src/database/ClipboardDatabase.cpp` | SQLite 剪贴板历史（独立连接 `clipboard`，`clipboard.db`，同内容去重） |
| SyncBridge | `include/ui/SyncBridge.h`, `src/ui/SyncBridge.cpp` | 设备同步主面板桥（Q_PROPERTY + Q_INVOKABLE + 工作线程跑引擎） |
| SyncEngine | `include/core/sync/SyncEngine.h`, `src/core/sync/SyncEngine.cpp` | 三方差集同步引擎（本地现状 / 上次基线 / 对端清单 → 唯一动作） |
| ITransport | `include/core/sync/ITransport.h` | 传输通道接口（`probe`/`listRemote`/`fetch`/`push`/`removeRemote`） |
| FolderTransport | `include/core/sync/FolderTransport.h`, `src/core/sync/FolderTransport.cpp` | 共享目录通道（异地靠网盘搬运，**新增依赖 0**） |
| ConflictResolver | `include/core/sync/ConflictResolver.h`, `src/core/sync/ConflictResolver.cpp` | `decide(L,S,R)` 纯函数判定表 + 冲突副本命名 |
| PathGuard | `include/core/sync/PathGuard.h`, `src/core/sync/PathGuard.cpp` | 相对路径安全校验 + junction 解引用后复核前缀 |
| SyncDatabase | `include/database/SyncDatabase.h`, `src/database/SyncDatabase.cpp` | SQLite 同步状态（独立连接 `sync`，`sync.db`：devices / sync_state / conflicts / received） |
| assistant_tests | `tests/test_sync.cpp` | QTest 单元测试（51 用例，含真文件系统端到端、同步锁与投递区） |

> **DB 约定**：每个功能一个独立 DB 文件 + 独立 `QSqlDatabase` 连接名（见 `ClipboardDatabase`），不要共用一个连接。
> **QML 约定**：搜索浮窗走 `AppBridge`，主面板走 `PanelBridge`；同步页走 `SyncBridge`（第三个桥，注入面板引擎）。
> **测试约定**：QTest（`Qt6::Test`），不是 GTest。`tests/*.cpp` 里要 `#include "<名字>.moc"` 让 AUTOMOC 处理 `Q_OBJECT`。

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
- `docs/architecture/` — 架构设计（**文件检索重构方案 / 文件治理功能设计 / v0.4-实施清单 / 设备同步功能-设计文档**）
- `docs/devlog/` — 开发日志（按日期）
- `docs/features/` — 功能设计 idea
- `docs/learning/` — 学习笔记
- `docs/troubleshooting/` — 踩坑记录
- `.obsidian/` — Obsidian 笔记配置
