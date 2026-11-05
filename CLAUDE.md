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
| 插件解压 | miniz (vcpkg, `find_package(miniz CONFIG REQUIRED)`；Qt6 无公开 zip API) |
| 配置路径 | `%LOCALAPPDATA%/AssistantTeam/Assistant/` |

## 编译命令

```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Debug
```

新增/删除源文件后必须重新 configure，因为用了 `file(GLOB_RECURSE)`。

> **另一台机器的写法**（同步模块开发机）：vcpkg 在 `D:\vcpkg`、生成器是 `Visual Studio 18 2026`、
> cmake 在 `D:\vcpkg\downloads\tools\cmake-4.4.3-windows-x86_64\bin\cmake.exe`（PATH 里没有）。
> 两者可替换：`-G` 与 `CMAKE_TOOLCHAIN_FILE` 跟着机器走，其余命令不变。

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

> **文件索引引擎（A7 起）**：v2 是**唯一**实现（`FileIndex` 门面 + 后台扫描 + A5 增量 + A6 冷启动），
> `fileIndex.engine` 开关已删除；旧 `FileSearcher` 只剩 `golden_file_index` 的对照实现，产品路径零调用。
> 子进程驱动应用做验收时用 `_probe/hotkey_probe.ps1`（`PostMessage(WM_HOTKEY)` 走真实消息链，见 `docs/devlog/2026-10-16.md`）。

## Qt 部署原理（vcpkg 开发阶段）

两层机制：
1. **qt.conf**（exe 同级，POST_BUILD 生成）：Qt 内部查找 plugins/qml → vcpkg `Qt6/plugins` `Qt6/qml`
2. **run.bat PATH**：Windows `LoadLibrary` 找 QML 插件依赖的 `Qt6*` DLL

`windeployqt` 在 vcpkg 版 Qt 不可用。

## 当前开发阶段

**v0.4.0 文件子系统 A 阶段** + **M4 插件市场已合并**（2026-11-04）

已完成：
- Phase 0: Hello 窗口、编译环境
- Phase 1 MVP: 系统托盘 + 全局热键 (Alt+Space) + QML 窗口 + 配置读写 + 主题
- Phase 2 (v0.2.0): 日志系统 / 搜索历史 SQLite / FuzzyMatcher 五级打分 / Calculator / AppIndexer 应用启动器 / FileSearcher 文件搜索 / QML 适配 / 集成测试
- Phase 3 主面板: 3.0 主面板骨架（左侧导航 + StackLayout 四页，与搜索浮窗并存）
- Phase 3 待办: `TodoDatabase` + `TodoPage`（记录 → 完成划去）
- Phase 3 记事本: `NotesDatabase` + `NotesPage`（CRUD + 编辑）
- Phase 3 剪贴板: `ClipboardDatabase` + `ClipboardPage` + `startClipboardMonitor`
- Phase 3 设置页: 主题切换 / 日志级别 / 当前热键显示
- **M4 插件市场（2026-11-04 合入 main）**：M0–M4 全链路 —— 本地插件管理、市场目录（内置 seed + 缓存 + HTTP）、
  本地 zip 与在线安装（zip-slip 防护 + sha256 校验）、启用/禁用/卸载、插件贡献页面进入左侧导航。
  主面板导航同时从 `StackLayout + selectedIndex` 改为 `Loader + selectedKey`（插件页是运行时才知道的）。

**设备同步（M2）· 渠道目录通道**（`docs/architecture/设备同步功能-设计文档.md`）：
- [x] `ITransport` 抽象 + `FolderTransport`（投递区 = 共享文件夹，**新增第三方依赖 0**）
- [x] `SyncEngine` 三方差集（本地现状 / 上次基线 / 对端清单 → 唯一动作）+ `ConflictResolver` 冲突副本
- [x] `PathGuard`（穿越 / 保留名 / junction 解引用后复核）+ `SyncDatabase`（`sync.db`：devices / sync_state / conflicts / received）
- [x] 主面板「设备同步」页 + `SyncBridge`（工作线程跑引擎，独立 DB 连接）
- [x] 快捷同步与投递区：拖拽 → 共享目录 `_投递/` → 对端轮询领取到收件箱 + 托盘通知
- [x] 多本地文件夹（`SyncRoot{localPath, alias}`）、同步锁、UNC 共享目录
- [x] QTest 用例（`tests/test_sync.cpp`，2026-10 时为 51 个）
- [ ] 冲突列表 UI、多文件夹版界面未真机走查
- [ ] 直连通道 `LanTransport` + 帧协议：**只有设计，没有代码**（分支内 `grep QTcpSocket` 零命中）

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
- [x] **A3 采集**（2026-10-22）：`FileScanner`（Win32 `FindFirstFileExW` **句柄栈** DFS，顺序与 A1 递归逐条相同）+
      `FileIndexDatabase`（dirs/files/roots + 三方差集差量）+ `DatabaseMigrator`（user_version 前向迁移，v1→v2 加治理字段）。
      四条验收全过（吞吐 6.7 万文件/s、剪枝 97.7%、迁移 14 项、差集收敛 0）。详见 `docs/devlog/2026-10-22.md`
- [x] **A4 查询 + baseline**（2026-10-22）：`QueryParser`（自由词 AND + `-否定`）+ `ScoreEngine`（tier 判定 +
  **贪心 O(m) 子序列** + 有界编辑距离，全程零分配）+ `QueryEngine`（top-K min-heap）；`FileIndex::query()` 瘦成转发。
  三条验收全过（语义 golden 全绿、baseline 落盘、与旧版对比 107 项无回归）。详见 `docs/devlog/2026-10-22.md`
- [x] **A5a 事件源**（2026-11-05）：`contract/FileSystemEvent.h`（C2）+ `FileWatcher`（OVERLAPPED，
      `start()` 返回即生效、按路径合并去重、溢出上报）+ `FileEventDispatcher`（订阅表，不是 EventBus）；
      8 个 QTest 用例。详见 `docs/devlog/2026-11-05.md`
- [x] **A5b 接入**（2026-11-05）：`FileIndex::setEventSource()` 订阅事件 → 60 ms 合并 → 一次性线程做
      "旧快照 + 事件 → 新快照 + 库差量" → 回到 GUI 线程发布；`Overflow` / 批 >2000 条 / 扫描中 → 全量重扫。
      `setRoots()` 让扫描根可注入（测试与将来的配置项）。端到端用例 `assistant_index_tests`：新建 3 s 内可搜到、
      删除后从结果消失。§A5 两条验收（新建/改名/删除可见、溢出后收敛）**已满足**
- [x] **A6a 冷启动先可用**（2026-11-05）：`startAsync()` 先载 `file_index.db` 发布快照（`cacheLoaded`），
      AppBridge 在这条信号上也刷新；随后扫描校正并发 `indexReady`。真机 32953 条 **265 ms** 可查
- [ ] **A6b 性能与 frecency**：265 ms/33k 线性外推到 20 万约 1.6 s，**未达 §9 的 <300 ms**；
      需要异步载入 + 列式/字典编码。frecency（`files.frecency` + 空查询 top-8）未做
- [x] **A7a 检索收尾**（2026-11-05）：`ISearchProvider` + `AppBridge` 的 **k-way merge**（每个 provider 自截 top-20，
      合并上限 50，堆按 score/priority 归并）；计算器改为 provider 内部的"先门禁后解析"；
      **删除 `fileIndex.engine` 开关**（main / ConfigManager / AppBridge 三处），v2 是唯一实现
- [ ] **A7b 剩余**：`FileSearcher` 仍被 `golden_file_index` 当作对照实现（产品路径零调用），删除需先重写该用例的
      phase 2；QML `Ctrl+Enter` 打开所在文件夹、`Shift+Enter` 复制路径未做；`fileIndex.debugScore` 未做
- [ ] A8 拼音首字母

> ✅ **P0-3 已修掉**：不再套 2 万条任意截断，真机索引 20000 → **31845** 条，
> `Documents`/`Downloads` 终于进得来。A3 的 Win32 扫描器扫 31845 条只要 **459–502 ms**。
> ✅ **查询已提速 2–8 倍**（A4，Release）：2 万条 1–3 字符 **3.9 ms**（旧 18.7，4.8×）、
> 20 万条 1–3 字符 **35.4 ms**（旧 275，7.8×）；4–8 字符档 1.8–2.0×。
> 冷启动到可查询 **28.7 ms**（§9 目标 <150 ms）。
> ⚠️ **但离 §9 目标仍差一个数量级**：20 万条 P50 35 ms vs 目标 3 ms；快照内存 **64.4 MB**
> 已过 60 MB 的"必须动手"线。baseline（`assistant_bench`）指向下一步：**§4.2 位图预筛** + **§3.1 目录字典编码**。
> ⚠️ **测性能必须用 Release**：同一份 2 万条查询，Debug 114 ms、Release 3.9 ms（差 30 倍）。

> 🐞 **部署缺口（打包前必须补）**：构建目录的 app-local 拷贝只拷 `Qt6*.dll`，
> **不含 `sqlite3.dll`**（`qsqlite.dll` 链接它）。Debug 一直被 `run.bat` / 调试命令里的 vcpkg PATH 掩盖，
> ctest 才暴露。已在 `a3_migrate` 的 ctest 环境里补 PATH；正式打包要显式带上。

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
>
> **例外（2026-11-04）**：M4 插件市场已提前合入 main（`docs/devlog/2026-11-04.md`）。
> 这是"代码先合、版本后追"：能力模型（capability 校验、宿主解析身份）与签名策略仍未做，
> 版本号仍按 `v0.6.0` 规划，**合并 ≠ 发布**。

## 核心模块

| 模块 | 文件 | 职责 |
|------|------|------|
| Logger | `include/core/Logger.h`, `src/core/Logger.cpp` | 命名空间级单例, 双 sink (控制台+文件滚动 5MB×3) |
| ConfigManager | `include/core/ConfigManager.h`, `src/core/ConfigManager.cpp` | JSON 配置读写, Q_PROPERTY 暴露给 QML |
| HistoryDatabase | `include/database/HistoryDatabase.h`, `src/database/HistoryDatabase.cpp` | SQLite 3 张表 (search/command/app_launch history) |
| AppBridge | `include/ui/AppBridge.h`, `src/ui/AppBridge.cpp` | QML↔C++ 桥梁；A7 起内部由 provider 组成（builtin / calc / files），k-way merge 出结果 |
| SearchResult | `include/ui/SearchResult.h` | 统一搜索模型 (id, title, subtitle, type, priority, score, resultText, path) |
| FuzzyMatcher | `include/core/FuzzyMatcher.h`, `src/core/FuzzyMatcher.cpp` | 五级模糊匹配打分 (精确>前缀>词边界>包含>编辑距离)。**A4 起只剩 `legacy` 退路与"跨模块分数尺度"的参照**，文件查询走 `ScoreEngine` |
| ScoreEngine | `include/core/ScoreEngine.h`, `src/core/ScoreEngine.cpp` | 打分内核：tier 判定（四档数值与 `FuzzyMatcher` **逐位相同**）+ **贪心 O(m) 子序列**（351–399 空档）+ 有界编辑距离；全程零分配 |
| QueryParser | `include/core/QueryParser.h`, `src/core/QueryParser.cpp` | A 阶段最小语法：自由词按空白 **AND**、`-词` 否定；整串只 `toLower` 一次 |
| QueryEngine | `include/core/QueryEngine.h`, `src/core/QueryEngine.cpp` | 查询引擎：Parser → 逐条打分 → **top-K min-heap**（替代全排序）；只依赖快照只读接口 |
| AppIndexer | `include/core/AppIndexer.h`, `src/core/AppIndexer.cpp` | 扫描 Start Menu `.lnk` (IShellLink COM), 解析目标路径, 去重排序 |
| FileSearcher | `include/core/FileSearcher.h`, `src/core/FileSearcher.cpp` | 自控前序 DFS（`walkDir`，不跟随 junction、目录级剪枝）+ FuzzyMatcher 打分 top-N, 索引上限 2 万。**A7 起产品路径零调用，仅剩 golden 的对照实现**（删除需先重写该用例） |
| FileSnapshot | `include/core/FileSnapshot.h` | 不可变只读快照（`Builder` 写入 → `nameAt/lowerAt/pathAt/sizeAt/mtimeAt` 读出）；物理表示藏在这层之后，查询层不摸容器 |
| SnapshotManager | `include/core/SnapshotManager.h`, `src/core/SnapshotManager.cpp` | 快照的 COW 发布（`std::atomic<std::shared_ptr<const T>>`）；读侧取一份即用，无需加锁 |
| FileIndex | `include/core/FileIndex.h`, `src/core/FileIndex.cpp` | 文件索引**门面**（薄）：`startAsync` 后台扫描 + `query` + `stats` + A5b 的 `setEventSource` / `setRoots`；`engine=v2` 时经 AppBridge 提供文件结果。扫描/写库都在 worker 线程（`QSqlDatabase` 线程亲和） |
| FileScanner | `include/core/FileScanner.h`, `src/core/FileScanner.cpp` | 采集层：Win32 **句柄栈 DFS**（`FindFirstFileExW` + `FindExInfoBasic` + `LARGE_FETCH`）；目录级剪枝、不跟随 reparse point、长路径 `\\?\`、权限拒绝计数、可取消。剪枝名单只有这一份 |
| FileIndexDatabase | `include/database/FileIndexDatabase.h`, `src/database/FileIndexDatabase.cpp` | `file_index.db`（独立连接 `file_index`）：dirs/files/roots 三表、三方差集、UPSERT 批量事务。**派生数据**，可删库重扫 |
| DatabaseMigrator | `include/database/DatabaseMigrator.h`, `src/database/DatabaseMigrator.cpp` | `PRAGMA user_version` **前向**迁移，每步一事务；高版本库拒绝；禁止 DROP+重建 |
| Calculator | `include/core/Calculator.h`, `src/core/Calculator.cpp` | 递归下降解析器, PEMDAS + 一元正负号 + 括号 |
| TrayManager | `include/ui/TrayManager.h`, `src/ui/TrayManager.cpp` | 系统托盘图标 + 右键菜单 |
| HotkeyManager | `include/core/HotkeyManager.h`, `src/core/HotkeyManager.cpp` | Windows RegisterHotKey 全局热键（可重注册） |
| PanelBridge | `include/ui/PanelBridge.h`, `src/ui/PanelBridge.cpp` | 主面板 QML↔C++ 桥梁（待办/笔记/剪贴板列表 + CRUD + 剪贴板监听） |
| TodoDatabase | `include/database/TodoDatabase.h`, `src/database/TodoDatabase.cpp` | SQLite 待办（独立连接 `todo`，`todo.db`） |
| NotesDatabase | `include/database/NotesDatabase.h`, `src/database/NotesDatabase.cpp` | SQLite 笔记（独立连接 `notes`，`notes.db`） |
| ClipboardDatabase | `include/database/ClipboardDatabase.h`, `src/database/ClipboardDatabase.cpp` | SQLite 剪贴板历史（独立连接 `clipboard`，`clipboard.db`，同内容去重） |
| PluginManager | `include/core/plugin/PluginManager.h`, `src/core/plugin/PluginManager.cpp` | M4 **门面**：唯一持有 Registry/Loader/Installer/Catalog 四个协作者，编排加载与安装，对外只发信号 |
| PluginRegistry | `include/core/plugin/PluginRegistry.h` | 插件安装目录布局 `plugins/<id>/`、`plugins.json` 清单；不加载代码 |
| PluginLoader | `include/core/plugin/PluginLoader.h` | QML 加载与页面对象生命周期；每个插件一个 `PluginHost`，卸载先销毁页面再删目录 |
| PluginHost | `include/core/plugin/PluginHost.h` | 插件**唯一**可见的宿主面：只读属性 + 自己的 `data/` + toast；不暴露引擎/注册表/管理器 |
| PluginInstaller | `include/core/plugin/PluginInstaller.h` | 安装流水线 `inspect→download→extract→validate→commit`；zip-slip 防护、sha256 校验、失败清理 |
| PluginCatalog | `include/core/plugin/PluginCatalog.h` | 市场索引：内置 `seed-index.json` → 本地缓存 → HTTP（`ASSISTANT_MARKET_URL`） |
| PluginBridge | `include/ui/PluginBridge.h`, `src/ui/PluginBridge.cpp` | 插件市场页 QML↔C++ 桥：列表、安装/取消/卸载、导航条目 |
| SyncBridge | `include/ui/SyncBridge.h`, `src/ui/SyncBridge.cpp` | 设备同步页的桥：Q_PROPERTY + Q_INVOKABLE，工作线程跑引擎，独立 DB 连接 |
| SyncEngine | `include/core/sync/SyncEngine.h`, `src/core/sync/SyncEngine.cpp` | 三方差集同步引擎（本地现状 / 上次基线 / 对端清单） |
| ITransport | `include/core/sync/ITransport.h` | 传输通道接口：`probe` / `listRemote` / `fetch` / `push` / `removeRemote` |
| FolderTransport | `include/core/sync/FolderTransport.h`, `src/core/sync/FolderTransport.cpp` | 共享目录通道（异地靠网盘搬运） |
| ConflictResolver | `include/core/sync/ConflictResolver.h` | `decide(L,S,R)` 纯函数判定表 + 冲突副本命名 |
| PathGuard | `include/core/sync/PathGuard.h` | 相对路径安全校验 + junction 解引用后复核前缀 |
| SyncDatabase | `include/database/SyncDatabase.h` | `sync.db`（独立连接 `sync`）：devices / sync_state / conflicts / received |
| FileSystemEvent | `include/contract/FileSystemEvent.h` | C2 契约：Added/Removed/Renamed/Modified/Overflow |
| FileWatcher | `include/core/FileWatcher.h`, `src/core/FileWatcher.cpp` | 每根一个 OVERLAPPED 句柄；`start()` 即生效；按路径合并去重；溢出显式上报 |
| FileEventDispatcher | `include/core/FileEventDispatcher.h` | 极轻量订阅表（不是全局 EventBus）；`publish` 在调用线程执行回调 |
| ISearchProvider | `include/core/ISearchProvider.h` | 检索来源接口（A7）：`query(text, limit) -> top-N`；内置 / 计算器 / 文件各一个实现 |

> **DB 约定**：每个功能一个独立 DB 文件 + 独立 `QSqlDatabase` 连接名（见 `ClipboardDatabase`），不要共用一个连接。
> **QML 约定**：搜索浮窗走 `AppBridge`，主面板走 `PanelBridge`，同步页走 `SyncBridge`（第三个桥，注入面板引擎）。
> **测试约定**：QTest（`Qt6::Test`），不是 GTest；`tests/*.cpp` 里要 `#include "<名字>.moc"` 让 AUTOMOC 处理 `Q_OBJECT`。
> **测试目标**：`assistant_tests`（同步）、`assistant_watch_tests`（A5 事件源）、`assistant_index_tests`（事件→索引→查询）、`assistant_search_tests`（provider + k-way merge），外加 `assistant_golden` / `assistant_a3` / `assistant_bench`；`ctest` 一次跑全 7 项。

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
- `docs/architecture/` — 架构设计（**文件检索重构方案 / 文件治理功能设计 / v0.4-实施清单**；
  `文件系统四模块-关联与方案.md` 是四模块（管理/传输/安全/插件）扩展的**提案 v2**：
  四项边界决策已定（网关只做授权协调 / 首版不做 mDNS / 插件进程内完全信任 / 保险箱只承诺静止态加密），
  **排期未变**，实现前须先验证 Qt TLS 后端可用性；
  `四模块完整架构.md` 是**架构基线 v1.0**：分工、契约 C1–C4、线程与存储、启动顺序、四条端到端时序、
  依赖矩阵与不变量清单（决策仍以提案为准，本文只答"是什么/谁负责/怎么接/现状到哪"）；
  `四分支整合方案.md` 是三条支线（plugin-market / qianju / file_guard）合并进 main 的顺序与裁剪清单；
  `分支整合流程.md` 是**长期整合规则**（有分支更新时先跑 `tools/branch-status.ps1` 对表，再按
  已合并 / 可合并 / 冲突 / 历史被改写四种状态分派；已推送分支禁止 rebase/force-push））
- `docs/reference/` — 参考实现与设计归档（`file_guard/`：M3 安全模块的 Python 原型文档，不进构建；
  可跑代码 + 115 个 unittest 在 `tools/file_guard/`，`run-tests.ps1` 一键跑）
- `docs/devlog/` — 开发日志（按日期）
- `docs/features/` — 功能设计 idea
- `docs/learning/` — 学习笔记
- `docs/troubleshooting/` — 踩坑记录
- `.obsidian/` — Obsidian 笔记配置
