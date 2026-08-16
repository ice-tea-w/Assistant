# 桌面助手软件开发流程指南

## 项目概述

使用 C++ 构建一款 Windows 桌面助手软件，同时学习 Git 版本控制、Docker 容器化等现代软件开发流程。

---

## 第一阶段：环境搭建与工具链

### 1.1 开发工具安装

| 工具                     | 用途                 | 安装方式                                                   |
| ---------------------- | ------------------ | ------------------------------------------------------ |
| **VS 2022 Community**  | C++ 编译器（MSVC）+ IDE | `winget install Microsoft.VisualStudio.2022.Community` |
| **CMake**              | 跨平台构建系统            | `winget install Kitware.CMake`                         |
| **Git**                | 版本控制（已安装）          | `winget install Git.Git`                               |
| **Visual Studio Code** | 代码编辑器              | `winget install Microsoft.VisualStudioCode`            |
| **vcpkg**              | C++ 包管理器           | `git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg` |

> **工具链说明：** 本项目采用 **VS2022 (MSVC) + CMake + vcpkg + Qt6** 单一工具链路线。
> 这是 Qt 官方对 Windows 平台最推荐的组合，避免 MSYS2/MinGW 与 MSVC 混用导致的 ABI 不兼容、DLL 问题和 vcpkg triplet 混乱。
> vcpkg 安装到 `C:\vcpkg`（官方推荐全局路径），初始化后执行 `C:\vcpkg\bootstrap-vcpkg.bat`，项目中使用 `x64-windows` triplet（MSVC 兼容）。
> **如果系统同时安装了 MSYS2**，CMake 可能误选 MinGW 编译器。必须加 `-G "Visual Studio 17 2022" -A x64` 强制指定 MSVC，详见 1.3 节。

### 1.2 推荐 VS Code 插件

先安装 VS Code：
```powershell
winget install Microsoft.VisualStudioCode
```

然后在 **PowerShell** 中通过命令行安装插件：
```powershell
code --install-extension ms-vscode.cpptools          # C++ 智能提示
code --install-extension ms-vscode.cmake-tools       # CMake 集成
code --install-extension twxs.cmake                  # CMake 语法高亮
code --install-extension eamodio.gitlens             # Git 增强
code --install-extension ms-azuretools.vscode-docker # Docker 支持
```

> 也可以在 VS Code 内按 `Ctrl+Shift+X` 打开扩展面板，直接搜索安装。

### 1.3 初始化项目结构

```bash
# 创建目录骨架
mkdir -p Assistant/src/core        # 核心逻辑（应用生命周期、配置管理）
mkdir -p Assistant/src/services    # 服务模块（热键、剪贴板、搜索等业务逻辑）
mkdir -p Assistant/src/ui          # 界面模块（QML 文件 + C++ Controller）
mkdir -p Assistant/src/plugins     # 插件系统（接口定义 + 插件加载器）
mkdir -p Assistant/src/utils       # 工具类（字符串、文件、通用辅助函数）
mkdir -p Assistant/src/database    # 数据库层（SQLite 封装、DAO）
mkdir -p Assistant/include         # 头文件
mkdir -p Assistant/tests           # 单元测试
mkdir -p Assistant/resources       # 资源文件（QML、图标、字体）
mkdir -p Assistant/docs            # 文档
mkdir -p Assistant/scripts         # 构建/部署脚本
mkdir -p Assistant/docker          # Docker CI 配置
mkdir -p Assistant/.github/workflows # CI/CD 配置
```

### 1.4 编译运行 Hello 窗口（Phase 0 核心）

```powershell
# 1. 配置 CMake（必须强制 MSVC 生成器，否则 PATH 中的 MSYS2/MinGW 会干扰）
cmake -B build -S . `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows

# 2. 编译
cmake --build build --config Debug

# 3. 运行（首次前阅读下方的 Qt 部署说明）
.\build\Debug\run.bat
```

### 1.5 Qt 部署：vcpkg 开发阶段 ⚠️ 重要踩坑记录

vcpkg 安装的 Qt 与官方 SDK 目录结构不同，`windeployqt` 不可用。QA 问题分为三层：

| 轮次  | 错误信息                                                             | 根因                                               | 解决方案                             |
| :-: | ---------------------------------------------------------------- | ------------------------------------------------ | -------------------------------- |
|  1  | `qt.qpa.plugin: Could not find the Qt platform plugin "windows"` | exe 找不到 `platforms/qwindowsd.dll`                | `qt.conf` 指定 `Plugins` 路径        |
|  2  | `module "QtQuick.Controls" is not installed`                     | QML 引擎找不到 QML 插件 DLL                             | `qt.conf` 指定 `QmlImports` 路径     |
|  3  | `Cannot load library qtquick2plugind.dll: 找不到指定的模块`              | QML 插件找到了，但 Windows 加载器找不到它的依赖 `Qt6Quickd.dll` 等 | `run.bat` 把 vcpkg `bin/` 加入 PATH |

**核心原理：两套查找机制各司其职**

| 机制 | 负责 | 配置方式 |
|------|------|------|
| Qt 内部路径 (`platforms/`、`qml/`) | `qt.conf` 在 exe 同级 | Qt 启动时自动读取 |
| Windows DLL 加载器 (`LoadLibrary`) | `PATH` 环境变量 | 运行时设置，加载 QML 插件依赖的 `Qt6*` DLL |

**CMakeLists.txt 中的实现：**

```cmake
# ---- Qt 部署 ----
# 开发阶段：POST_BUILD 自动生成 qt.conf 和 run.bat

# 1. 生成 qt.conf（Qt 内部路径）
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/write_qt_conf.cmake"
"file(WRITE \"\${OUT_FILE}\" \"[Paths]\\n\")
file(APPEND \"\${OUT_FILE}\" \"Prefix = \${QT_PREFIX}\\n\")
file(APPEND \"\${OUT_FILE}\" \"Plugins = Qt6/plugins\\n\")
file(APPEND \"\${OUT_FILE}\" \"QmlImports = Qt6/qml\\n\")
")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -D "OUT_FILE=$<TARGET_FILE_DIR:${PROJECT_NAME}>/qt.conf"
        -D "QT_PREFIX=$<IF:$<CONFIG:Debug>,C:/vcpkg/installed/x64-windows/debug,C:/vcpkg/installed/x64-windows>"
        -P "${CMAKE_CURRENT_BINARY_DIR}/write_qt_conf.cmake"
    COMMENT "Generating qt.conf (vcpkg Qt6)"
)

# 2. 生成 run.bat（自动设置 PATH 后启动）
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/write_run_bat.cmake"
"file(WRITE \"\${OUT_FILE}\" \"@echo off\\n\")
file(APPEND \"\${OUT_FILE}\" \"set \\\"PATH=\${QT_PATH}/bin;%PATH%\\\"\\n\")
file(APPEND \"\${OUT_FILE}\" \"cd /d \\\"\${EXE_DIR}\\\"\\n\")
file(APPEND \"\${OUT_FILE}\" \"start \\\"\\\" \\\"\${EXE_NAME}\\\"\\n\")
")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -D "OUT_FILE=$<TARGET_FILE_DIR:${PROJECT_NAME}>/run.bat"
        -D "QT_PATH=$<IF:$<CONFIG:Debug>,C:/vcpkg/installed/x64-windows/debug,C:/vcpkg/installed/x64-windows>"
        -D "EXE_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        -D "EXE_NAME=$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        -P "${CMAKE_CURRENT_BINARY_DIR}/write_run_bat.cmake"
    COMMENT "Generating run.bat"
)

# 发布阶段（打包时替换以上所有部署代码）：
# qt_generate_deploy_app_script(
#     TARGET ${PROJECT_NAME}
#     OUTPUT_SCRIPT deploy_script
#     NO_UNSUPPORTED_PLATFORM_ERROR
# )
# install(SCRIPT ${deploy_script})
```

> **发布阶段** 使用 `qt_generate_deploy_app_script()`（Qt6 CMake Deployment API），自动收集所有 DLL + plugins + QML 模块到安装目录。开发阶段不要用，每次编译等几分钟不划算。

---

## 第二阶段：Git 版本控制入门

### 2.1 Git 基础概念

```
工作区 (Working Directory)    ← 你编辑代码的地方
    ↓ git add
暂存区 (Staging Area)         ← 准备提交的改动
    ↓ git commit
本地仓库 (Local Repository)   ← 本地的版本历史
    ↓ git push
远程仓库 (Remote Repository)  ← GitHub / GitLab 等
```

### 2.2 常用 Git 命令速查

```bash
# === 配置 ===
git config --global user.name "ice_tea"
git config --global user.email "your@email.com"

# === 日常开发流程 ===
git status                    # 查看当前状态
git add <file>                # 将文件加入暂存区
git add -A                    # 暂存所有改动
git commit -m "描述你的改动"   # 提交到本地仓库
git push origin master        # 推送到远程

# === 分支操作 ===
git branch                    # 查看所有分支
git branch feature/xxx        # 创建新分支
git switch feature/xxx        # 切换到分支
git switch -c feature/xxx     # 创建并切换
git merge feature/xxx         # 合并分支到当前分支
git branch -d feature/xxx     # 删除分支

# === 查看历史 ===
git log --oneline --graph     # 图形化查看历史
git diff                      # 查看未暂存的改动
git diff --staged             # 查看已暂存的改动
git show <commit-hash>        # 查看某次提交详情

# === 撤销操作 ===
git restore <file>            # 撤销工作区改动
git restore --staged <file>   # 取消暂存
git reset --soft HEAD~1       # 撤销最近一次 commit（保留改动）
git revert <commit-hash>      # 安全地撤销某次提交

# === 远程操作 ===
git remote add origin <url>   # 关联远程仓库
git fetch origin              # 获取远程更新（不合并）
git pull origin master        # 拉取并合并远程更新
```

### 2.3 Git 分支策略

个人项目推荐 **Trunk-Based Flow（简化版）**，新手也可以选择传统 Git Flow 作为学习过程。

#### 方案 A：Trunk-Based（推荐，效率优先）

```
main              ← 始终可发布
 ├── feature/hotkey    ← 功能分支（从 main 分出）
 ├── feature/search    ← 功能分支
 ├── fix/tray-click    ← Bug 修复分支
 └── feature/database  ← 功能分支
```

开发流程：`main` 上开 feature 分支 → 开发 → PR → merge 回 `main`。不维护 `develop` 分支，减少个人项目的额外成本。

#### 方案 B：Git Flow（学习优先）

```
master (main)     ← 稳定发布版本
  ├── develop     ← 日常开发主线
  │   ├── feature/xxx  ← 功能开发（从 develop 分出）
  │   ├── fix/xxx      ← Bug 修复（从 develop 分出）
  │   └── refactor/xxx ← 代码重构
  └── release/x.x  ← 发布准备分支
```

> 两者的核心区别：Trunk-Based 没有 `develop` 中间层，直接在 `main` 上操作。对于一个人开发的项目，Git Flow 更多是学习练习；如果以交付效率优先，选方案 A 即可。

### 2.4 提交信息规范

采用 Conventional Commits 规范：

```
<type>(<scope>): <描述>

类型(type):
  feat     - 新功能
  fix      - Bug 修复
  docs     - 文档更新
  style    - 代码格式调整（不影响功能）
  refactor - 重构（既不修复 bug 也不添加功能）
  test     - 测试相关
  chore    - 构建/工具链相关
  perf     - 性能优化

示例:
  feat(core): 添加全局快捷键监听模块
  fix(tray): 修复托盘图标点击无响应的问题
  docs(readme): 更新编译环境配置说明
```

---

## 第三阶段：C++ 项目架构设计

### 3.1 技术选型

| 模块       | 技术方案                    | 说明                                  |
| -------- | ----------------------- | ----------------------------------- |
| GUI 框架   | **Qt 6 Quick (QML)**    | 声明式 UI，动画原生支持，兼容 Qt Design Studio   |
| QML 控件   | **Qt Quick Controls 2** | 提供 Button、TextField 等现成 QML 控件      |
| 构建系统     | **CMake 3.20+**         | 现代 C++ 标准构建工具                       |
| 包管理      | **vcpkg**               | 微软官方的 C++ 包管理器                      |
| JSON 解析  | **nlohmann/json**       | 轻量级 JSON 库                          |
| HTTP 客户端 | **cpp-httplib**         | 轻量级 HTTP 库                          |
| 数据库      | **SQLite3**             | 嵌入式数据库                              |
| 测试框架     | **Google Test**         | C++ 单元测试框架                          |
| 日志       | **spdlog**              | 高性能日志库（统一输出到 `logs/assistant.log`）  |
| 热键       | **Windows API**         | 系统级全局热键（`RegisterHotKey`）           |
| 自动更新     | **WinSparkle**          | Windows 自动更新框架（v0.5+ 引入）            |
| 崩溃报告     | **Crashpad**            | Chromium 崩溃报告库，生成 .dmp 文件（v0.3+ 引入） |
| 安装包      | **NSIS**                | 制作 Windows 安装包（v0.5+ 引入）            |

### 3.2 核心功能模块

```
桌面助手 (Desktop Assistant)
│
├── 系统托盘 (System Tray)
│   ├── 托盘图标管理
│   ├── 右键菜单
│   └── 气泡通知
│
├── 全局热键 (Global Hotkeys)
│   ├── 呼出/隐藏主窗口
│   ├── 截图快捷键
│   └── 自定义热键绑定
│
├── 主界面 (QML Main Window)
│   ├── QML 声明式 UI 层（布局 + 动画）
│   ├── C++ 后端逻辑层（命令解析、数据处理）
│   ├── 搜索/命令输入框
│   ├── 结果展示列表
│   └── 设置面板
│
├── 插件系统 (Plugin System)
│   ├── 插件接口定义
│   ├── 动态加载 (.dll)
│   └── 插件管理
│
├── 功能模块 (Features)
│   ├── 剪贴板管理
│   ├── 快速启动应用
│   ├── 文件搜索
│   ├── 计算器
│   ├── 天气查询
│   ├── 待办事项
│   └── 笔记速记
│
├── 运维模块 (Operations)
│   ├── 自动更新 (WinSparkle)
│   └── 崩溃报告 (Crashpad, .dmp 生成与上传)
│
└── 基础设施 (Infrastructure)
    ├── 配置管理 (JSON)
    ├── 本地数据库 (SQLite)
    ├── 网络请求 (HTTP)
    └── 日志系统 (spdlog → logs/assistant.log)
```

### 3.3 CMakeLists.txt 模板

```cmake
cmake_minimum_required(VERSION 3.20)
project(Assistant VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)      # Qt MOC 自动处理
set(CMAKE_AUTORCC ON)      # Qt 资源文件自动处理（QML 文件通过 .qrc 打包）

# 查找 Qt6（QML 方案）
find_package(Qt6 REQUIRED COMPONENTS
    Core Quick QuickControls2 Network
)

# 查找其他依赖
find_package(SQLite3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

# 源文件
file(GLOB_RECURSE SOURCES src/*.cpp)
file(GLOB_RECURSE HEADERS include/*.h)

# QML 资源文件（QML 文件统一放在 resources/qml/ 下，通过 .qrc 引用）
# 示例：resources/resources.qrc 中包含:
#   <qresource prefix="/">
#     <file>qml/MainWindow.qml</file>
#     <file>qml/SearchBar.qml</file>
#   </qresource>

# 主程序
add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
    resources/resources.qrc
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    SQLite::SQLite3
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# ---- Qt 部署（详见 1.5 节） ----
# 开发阶段：qt.conf + run.bat（POST_BUILD 自动生成）
# 发布阶段：qt_generate_deploy_app_script()

# 测试
enable_testing()
add_executable(assistant_tests
    tests/test_main.cpp
    tests/test_config.cpp
    # ... 更多测试文件
)
target_link_libraries(assistant_tests PRIVATE
    GTest::gtest_main
    # 链接需要测试的库
)
include(GoogleTest)
gtest_discover_tests(assistant_tests)
```

### 3.4 vcpkg 依赖管理

```json
// vcpkg.json
{
  "name": "assistant",
  "version": "0.1.0",
  "dependencies": [
    "qt6",           // 包含 qtbase + qtdeclarative + qtquickcontrols2 等
    "sqlite3",
    "spdlog",
    "nlohmann-json",
    "cpp-httplib",
    "gtest",
    "clip"
  ]
}
```

---

## 第四阶段：分步开发计划

> **时间估算说明：** 以下为个人利用业余时间开发的估算。第一次配置 Qt + CMake + vcpkg + QML 通信会踩坑，每个阶段预留了缓冲时间。

### Phase 0 — 环境搭建  [Week 1]

**目标：** 跑通第一个窗口

- [x] 安装 VS2022 Community（勾选"使用 C++ 的桌面开发"工作负载）
- [x] 安装 Qt6（通过 Qt Online Installer 或 vcpkg）
- [x] 配置 CMake（3.20+）
- [x] 初始化 Git 仓库并推送到 GitHub
- [x] 用 CMake + Qt6 QML 编译运行一个空白窗口

**里程碑：** 屏幕上出现 "Hello Assistant" 窗口。

### Phase 1 — MVP：最小可行原型  [Week 2-4]

**目标：** 助手"能出现、能隐藏"

- [x] 系统托盘图标（含右键菜单：显示/退出）
- [x] 全局热键注册（如 Alt+Space 呼出/隐藏窗口）
- [x] QML 主窗口（搜索框 + 结果列表占位）
- [x] QML 与 C++ 后端的基础通信（信号/槽）
- [x] 配置文件读写（JSON：热键绑定、窗口位置、主题）

**里程碑：** `v0.1.0` — 按快捷键弹出窗口，托盘图标正常，配置可保存。

**Git 操作实践：**
```bash
git switch -c feature/mvp-tray
# ... 编写代码 ...
git add -A
git commit -m "feat(ui): 实现系统托盘图标和基础窗口"
git push origin feature/mvp-tray
# 在 GitHub 上创建 Pull Request → 合并到 main
```

### Phase 2 — 核心助手功能  [Week 5-8]

**目标：** 助手"能用起来"

- [ ] 搜索/命令输入框 + 命令解析器（输入关键词匹配功能）
- [ ] 应用快速启动（输入 `chrome` → 打开 Chrome）
- [ ] 简易计算器（输入 `1+2*3` → 输出结果）
- [ ] 本地文件搜索（基于文件名索引）
- [x] SQLite 数据持久化（搜索历史、应用快捷方式）
- [x] 日志系统（spdlog，写入 `logs/assistant.log`）

**里程碑：** `v0.2.0` — 可以用快捷键呼出、输入命令、执行操作。

#### 2.1 日志系统（spdlog）

> **已完成的 Step 1**（2026-08-06）

##### 技术选型

| 项目 | 选择 | 说明 |
|------|------|------|
| 日志库 | **spdlog**（vcpkg） | 高性能 C++ 日志，双 sink（控制台 + 文件），支持运行时切换级别 |
| fmt 兼容 | `SPDLOG_FMT_EXTERNAL` | vcpkg 构建 spdlog 时自动启用，链接 vcpkg 统一管理的 `fmt`，与 Qt6 无冲突 |
| 封装方式 | `Logger` 命名空间级单例 | 非 QObject，日志是基础设施不面向 QML，保持轻量 |
| 文件策略 | `rotating_file_sink_mt` | 5 MB × 3 个文件滚动，路径 `%LOCALAPPDATA%/AssistantTeam/Assistant/logs/` |
| 日志级别 | Debug → `debug`，Release → `info` | 配置文件 `logging.level` 控制，运行时可通过 `setLevel()` 动态切换 |

##### 新增文件

```
include/core/Logger.h       — 单例接口：init() / get() / setLevel()
src/core/Logger.cpp         — 双 sink 实现 + fallback 保护
```

##### 修改文件

| 文件 | 变更 |
|------|------|
| `CMakeLists.txt` | 取消注释 `find_package(spdlog REQUIRED)`，`target_link_libraries` 新增 `spdlog::spdlog` |
| `include/core/ConfigManager.h` | 新增 `Q_PROPERTY(QString logLevel ...)` + getter/setter/signal |
| `src/core/ConfigManager.cpp` | JSON 读写 `"logging": { "level": "..." }`，默认 `"info"` |
| `src/main.cpp` | `Logger::init(logDir, config.logLevel())` 在 ConfigManager load 之后调用 |
| `src/core/HotkeyManager.cpp` | `qDebug()`/`qWarning()` → `Logger::get().debug()`/`.warn()` |
| `src/core/ConfigManager.cpp` | `qInfo()`/`qWarning()` → `Logger::get().info()`/`.warn()` |

##### 使用方式

```cpp
// main.cpp — 一次性初始化（在 ConfigManager::load() 之后）
QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                 + "/logs";
Logger::init(logDir.toStdString(), config.logLevel().toStdString());
Logger::get().info("Assistant v{} starting", "0.1.0");

// 其他任何地方 — 直接调用
#include "core/Logger.h"
Logger::get().debug("HotkeyManager: registered {}+{}", mod, key);
Logger::get().warn("HistoryDatabase: cannot open {}", path);
```

#### 2.2 SQLite 搜索历史

> **已完成的 Step 2**（2026-08-06）

##### 技术选型

| 项目 | 选择 | 说明 |
|------|------|------|
| 数据库 | **Qt6::Sql**（内置 SQLite 驱动） | 无需额外 vcpkg 包，`QSqlDatabase`/`QSqlQuery` 是 Qt 惯用写法 |
| 连接名 | `"history"` | 独立命名，不干扰其他模块可能的数据库使用 |
| 模式 | WAL + synchronous=NORMAL | 支持并发读写，适合单线程应用的频繁记录 |
| 路径 | `%LOCALAPPDATA%/AssistantTeam/Assistant/history.db` | 与 `config.json` 同级 |
| 清理策略 | `cleanup(90)` | 自动删除 90 天前的搜索记录 |

##### 新增文件

```
include/database/HistoryDatabase.h   — QObject 封装，3 张表 API
src/database/HistoryDatabase.cpp     — schema 创建 + CRUD + 定期清理
```

##### 数据库表结构

```sql
-- 搜索记录（每次用户输入 ≥2 字符时记录）
CREATE TABLE search_history (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    query         TEXT    NOT NULL,
    result_count  INTEGER NOT NULL DEFAULT 0,
    created_at    TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
);

-- 命令执行计数（ON CONFLICT 自动递增 execution_count）
CREATE TABLE command_history (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    command_id      TEXT    NOT NULL UNIQUE,
    display_name    TEXT    NOT NULL,
    execution_count INTEGER NOT NULL DEFAULT 1,
    last_executed   TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
);

-- 应用启动频次（优先显示常用应用）
CREATE TABLE app_launch_history (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    app_name      TEXT    NOT NULL,
    app_path      TEXT    NOT NULL UNIQUE,
    launch_count  INTEGER NOT NULL DEFAULT 1,
    last_launched TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
);
```

##### 修改文件

| 文件 | 变更 |
|------|------|
| `CMakeLists.txt` | `find_package Qt6` 新增 `Sql` 组件；`target_link_libraries` 新增 `Qt6::Sql` |
| `include/ui/AppBridge.h` | 新增 `setHistoryDatabase(HistoryDatabase*)` + 成员指针 |
| `src/ui/AppBridge.cpp` | `search()` 末尾记录搜索（≥2 字符）；`executeCommand()` 记录命令执行 |
| `src/main.cpp` | 实例化 `HistoryDatabase` → `open(dbPath)` → 传入 `AppBridge` |

##### 调用链

```
用户输入 "notepad" + Enter
  → AppBridge::executeCommand(0)
    → executeAction("Open Notepad")
    → m_historyDb->recordCommandExecution("cmd://opennotepad", "Open Notepad")
      → SQL: INSERT ... ON CONFLICT DO UPDATE execution_count + 1
```

#### 2.3 后续 Step

Phase 2 剩余任务（Step 3-8）按计划推进：

| Step   | 内容                   | 说明                                             | 状态  |
| ------ | -------------------- | ---------------------------------------------- | :-: |
| Step 3 | CommandItem + 模糊匹配引擎 | 结构化结果模型，替代 `QStringList`，支持 Levenshtein 编辑距离匹配 |  ✅  |
| Step 4 | Calculator 简易计算器     | 递归下降解析器，支持 PEMDAS                              |  ✅  |
| Step 5 | AppIndexer 应用启动器     | 扫描 Start Menu `.lnk` 文件                        |  ✅  |
| Step 6 | FileSearcher 文件搜索    | QDirIterator 文件名搜索                             |  ✅  |
| Step 7 | QML 界面适配             | delegate 支持 title + subtitle + 类型图标            |  ✅  |
| Step 8 | 集成测试                 | 编译运行全功能验证                                      |  ✅  |

> 各 Step 的详细实现见 `docs/devlog/`（Step 3-4 → `2026-08-11.md`，Step 5 → `2026-08-13.md`，Step 6 → `2026-08-15.md`，Step 7-8 → `2026-08-16.md`）。

### Phase 3 — 扩展功能  [Week 9-16]

**目标：** 助手"变好用"

- [ ] 剪贴板历史管理
- [ ] 待办事项（SQLite 存储，QML 列表界面）
- [ ] 笔记速记
- [ ] 设置面板（QML 界面：热键自定义、主题切换、开机启动）
- [ ] 异常处理完善 + 崩溃报告（Crashpad，生成 .dmp 文件）
- [ ] 性能优化（启动速度、内存占用）

**里程碑：** `v0.3.0` — 功能完整，可日常使用。

### Phase 4 — 插件系统 + 发布  [Week 17+]

**目标：** 可扩展、可发布

> **注意：** 插件系统需要核心架构稳定后再做，否则接口会频繁变动。建议 v0.3 之后引入。

- [ ] 定义插件接口（纯虚基类：`init()`、`name()`、`shutdown()`）
- [ ] 动态库加载机制（`QLibrary` / Windows `LoadLibrary`）
- [ ] 开发 2-3 个示例插件（如 Weather、Translator、Calculator）
- [ ] 插件管理 UI
- [ ] 自动更新机制（WinSparkle）
- [ ] 安装包制作（NSIS 或 WiX Toolset）
- [ ] 编写用户文档

**里程碑：** `v0.5.0` — 可扩展架构，可分发安装包。

---

## 第五阶段：Docker 容器化（CI 辅助）

> **重要定位：** 本项目是 Windows 桌面 GUI 程序（.exe），Docker 无法运行 GUI。
> Docker 在本项目中的唯一用途是 **CI/CD 自动化构建与测试**。不要前期投入大量时间，等到有 CI 需求时再深入。

### 5.1 Docker 在 C++ 桌面项目中的角色

| 场景 | 说明 | 本项目的使用 |
|------|------|-------------|
| **CI/CD 构建** | 在 GitHub Actions 等 CI 中自动化编译和测试 | ✅ 主要用途 |
| **统一编译环境** | 确保构建环境一致（可选，Windows runner 通常不用） | ⚠️ 非必须 |
| **交叉编译** | 在 Linux 容器中交叉编译 Windows 程序 | ❌ 不建议，直接用 windows-latest runner |
| **服务端组件** | 如果助手将来有后端服务 | ❌ 暂无计划 |

> **推荐顺序：** 先写代码 → 软件能跑 → 写测试 → 再上 Docker CI。
> 不要一上来就研究 Docker，容易浪费大量时间。

### 5.2 Docker 常用命令速查

```bash
# === 镜像操作 ===
docker images                  # 列出所有本地镜像
docker pull <image>:<tag>      # 拉取镜像
docker build -t name:tag .     # 从 Dockerfile 构建镜像
docker rmi <image>             # 删除镜像

# === 容器操作 ===
docker run -it <image> bash    # 交互式运行容器
docker run -d <image>          # 后台运行
docker ps                      # 查看运行中的容器
docker ps -a                   # 查看所有容器（包括停止的）
docker stop <container>        # 停止容器
docker rm <container>          # 删除容器
docker exec -it <container> bash  # 进入运行中的容器

# === 数据与网络 ===
docker volume ls               # 列出数据卷
docker volume create <name>    # 创建数据卷
docker network ls              # 列出网络

# === Docker Compose ===
docker compose up -d           # 启动多容器应用
docker compose down            # 停止并删除
docker compose logs -f         # 查看日志
docker compose ps              # 查看服务状态
```

### 5.3 编译环境 Dockerfile

```dockerfile
# docker/Dockerfile.build
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 安装编译工具链
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    curl \
    zip \
    pkg-config \
    # Qt6 开发库
    qt6-base-dev \
    qt6-base-dev-tools \
    # 其他依赖
    libsqlite3-dev \
    libspdlog-dev \
    nlohmann-json3-dev \
    libgtest-dev \
    # 交叉编译 Windows（可选）
    mingw-w64 \
    && rm -rf /var/lib/apt/lists/*

# 安装 vcpkg
RUN git clone https://github.com/Microsoft/vcpkg.git /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh

ENV VCPKG_ROOT=/opt/vcpkg
ENV CMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake

WORKDIR /workspace
```

### 5.4 CI 编译环境（Docker Compose）

> 仅用于 CI/自动化构建场景。日常开发直接在 VS2022 中编译即可。

```yaml
# docker/docker-compose.yml
version: '3.8'

services:
  build:
    build:
      context: ..
      dockerfile: docker/Dockerfile.build
    volumes:
      - ..:/workspace
    working_dir: /workspace
    command: |
      bash -c "
        cmake -B build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake
        cmake --build build
        cd build && ctest --output-on-failure
      "

  # 如果之后需要后端服务
  # backend:
  #   build:
  #     context: ..
  #     dockerfile: docker/Dockerfile.backend
  #   ports:
  #     - "8080:8080"
```

### 5.5 日常 Docker 使用流程

```bash
# 1. 构建编译镜像
docker build -t assistant-build -f docker/Dockerfile.build .

# 2. 在容器中编译项目
docker run --rm -v ${PWD}:/workspace assistant-build \
    bash -c "cmake -B build && cmake --build build"

# 3. 在容器中运行测试
docker run --rm -v ${PWD}:/workspace assistant-build \
    bash -c "cd build && ctest --output-on-failure"

# 4. 一键构建+测试（使用 docker compose）
cd docker && docker compose run --rm build
```

---

## 第六阶段：CI/CD 自动化

### 6.1 GitHub Actions 配置

```yaml
# .github/workflows/build.yml
name: Build and Test

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  build:
    runs-on: windows-latest

    steps:
      - uses: actions/checkout@v4

      - name: Setup vcpkg
        run: |
          git clone https://github.com/Microsoft/vcpkg.git C:/vcpkg
          C:/vcpkg/bootstrap-vcpkg.bat

      - name: Configure and Build
        run: |
          cmake -B build -S . \
            -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
          cmake --build build --config Release

      - name: Run Tests
        run: |
          cd build
          ctest --output-on-failure -C Release

      - name: Package
        run: |
          cmake --install build --config Release --prefix install
```

### 6.2 版本标签自动化

```bash
# 打版本标签
git tag -a v0.1.0 -m "首个 MVP 版本"
git push origin v0.1.0

# 查看所有标签
git tag -l

# 切回到某个标签
git checkout v0.1.0
```

---

## 第七阶段：学习路线图

### 分阶段学习计划

```
Phase 0 (Week 1):  环境搭建
                    ├── VS2022 + Qt6 + CMake + vcpkg 安装配置
                    └── Git 基础 (init, add, commit, push, pull)

Phase 1 (Week 2-4):  MVP
                    ├── CMake 入门 + Qt6 QML 基础窗口
                    ├── 系统托盘 + 全局热键（Windows API）
                    └── C++/QML 信号槽通信

Phase 2 (Week 5-8):  核心功能
                    ├── spdlog 日志系统（Logger 单例，双 sink）
                    ├── SQLite 搜索历史（Qt6::Sql，3 张表）
                    ├── 命令解析器设计（SearchResult 模型 + 模糊匹配）
                    ├── Calculator / AppIndexer / FileSearcher
                    └── Git 分支与合并操作

Phase 3 (Week 9-16): 扩展功能
                    ├── C++ 设计模式（单例、观察者、策略）
                    ├── 剪贴板 + 文件搜索
                    ├── 异常处理 + 崩溃报告 (Crashpad)
                    └── Git 高级操作 (rebase, cherry-pick, stash)

Phase 4 (Week 17+):  插件系统 + 发布
                    ├── 动态库加载机制 (QLibrary / LoadLibrary)
                    ├── 插件接口设计 (ABI 兼容性)
                    ├── CI/CD + GitHub Actions + Docker
                    ├── 安装包制作 (NSIS)
                    └── 自动更新 (WinSparkle) + 项目复盘
```

### 推荐学习资源

| 主题 | 资源 |
|------|------|
| Git | [Pro Git 中文版](https://git-scm.com/book/zh/v2)、[Learn Git Branching](https://learngitbranching.js.org/) |
| CMake | [Modern CMake](https://cliutils.gitlab.io/modern-cmake/)、[CMake 官方教程](https://cmake.org/cmake/help/latest/guide/tutorial/) |
| Docker | [Docker 从入门到实践](https://yeasy.gitbook.io/docker_practice/)、[Play with Docker](https://labs.play-with-docker.com/) |
| Qt 6 | [Qt 官方文档](https://doc.qt.io/qt-6/)、[QML 入门](https://doc.qt.io/qt-6/qtqml-index.html)、[Qt Quick Controls 2](https://doc.qt.io/qt-6/qtquickcontrols2-index.html) |
| C++ | [CppReference](https://zh.cppreference.com/)、[C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/) |
| 设计模式 | 《Head First 设计模式》、[refactoring.guru](https://refactoring.guru/design-patterns) |

---

## 附录：快捷键与效率工具

### VS Code 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+Shift+P` | 命令面板 |
| `Ctrl+P` | 快速打开文件 |
| `Ctrl+Shift+F` | 全局搜索 |
| `Ctrl+`` | 打开终端 |
| `Ctrl+B` | 切换侧栏 |
| `F5` | 调试运行 |
| `F9` | 设置断点 |
| `Ctrl+K Ctrl+C` | 注释/取消注释 |

### 常用终端别名（添加到 `~/.bashrc`）

```bash
alias gs='git status'
alias ga='git add -A'
alias gc='git commit -m'
alias gp='git push origin $(git branch --show-current)'
alias gl='git log --oneline --graph -15'
alias gsw='git switch'
alias gswc='git switch -c'
alias dc='docker compose'
alias dcb='docker compose build'
alias dcu='docker compose up -d'
alias dcd='docker compose down'
```

---

> **下一步：** 按照第一阶段开始搭建环境，每完成一个 Phase 就打一个 Git Tag，并提交代码到 GitHub。

> 这篇文章会随着项目进展持续更新。每当学到新东西，就补充到对应章节中。
