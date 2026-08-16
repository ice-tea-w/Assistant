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

## 运行方式

```powershell
.\build\Debug\run.bat
```

`run.bat` 由 CMake POST_BUILD 自动生成，负责：
- 把 vcpkg Qt6 `bin/` 加入 PATH（Windows DLL 加载器需要）
- 在 exe 目录启动程序

## Qt 部署原理（vcpkg 开发阶段）

两层机制：
1. **qt.conf**（exe 同级，POST_BUILD 生成）：Qt 内部查找 plugins/qml → vcpkg `Qt6/plugins` `Qt6/qml`
2. **run.bat PATH**：Windows `LoadLibrary` 找 QML 插件依赖的 `Qt6*` DLL

`windeployqt` 在 vcpkg 版 Qt 不可用。

## 当前开发阶段

**Phase 2 进行中**（核心助手功能）

已完成：
- Phase 0: Hello 窗口、编译环境
- Phase 1 MVP: 系统托盘 + 全局热键 (Alt+Space) + QML 窗口 + 配置读写 + 主题
- Phase 2 Step 1: spdlog 日志系统 (Logger 命名空间单例)
- Phase 2 Step 2: SQLite 搜索历史 (HistoryDatabase, 3 张表)
- Phase 2 Step 3: SearchResult + 模糊匹配引擎 (FuzzyMatcher 五级打分)
- Phase 2 Step 4: Calculator 递归下降解析器 (PEMDAS)
- Phase 2 Step 5: AppIndexer 应用启动器 (扫描 Start Menu .lnk, IShellLink 解析)
- Phase 2 Step 6: FileSearcher 文件搜索 (QDirIterator 索引用户目录文件名, 模糊匹配 top-N)
- Phase 2 Step 7: QML 界面适配 (类型彩色徽章 + 键盘导航滚动跟随)
- Phase 2 Step 8: 集成测试 (端到端验证 + v0.2.0 里程碑)

下一步：Phase 3 — 扩展功能（剪贴板/待办/设置面板）

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
| FileSearcher | `include/core/FileSearcher.h`, `src/core/FileSearcher.cpp` | QDirIterator 递归索引用户目录文件名, FuzzyMatcher 打分 top-N, 索引上限 2 万 |
| Calculator | `include/core/Calculator.h`, `src/core/Calculator.cpp` | 递归下降解析器, PEMDAS + 一元正负号 + 括号 |
| TrayManager | `include/ui/TrayManager.h`, `src/ui/TrayManager.cpp` | 系统托盘图标 + 右键菜单 |
| HotkeyManager | `include/core/HotkeyManager.h`, `src/core/HotkeyManager.cpp` | Windows RegisterHotKey 全局热键 |

## Git 规范

- Conventional Commits: `feat(scope):`, `fix(scope):`, `docs:`, `chore:` 等
- Trunk-Based Flow: `main` 上开 feature 分支 → PR → merge

## 关键踩坑记录

详见 `docs/troubleshooting/` 目录：
- `cmake-glob-recurse-新文件不编译.md`
- `qt-automoc-未生成moc文件.md`
- `msvc-中文utf8编码错误.md`
- `vs2022-build-tools-安装.md`

## 文档结构

- `GUIDE.md` — 完整开发流程指南（长期维护）
- `docs/devlog/` — 开发日志（按日期）
- `docs/features/` — 功能设计 idea
- `docs/learning/` — 学习笔记
- `docs/troubleshooting/` — 踩坑记录
- `.obsidian/` — Obsidian 笔记配置
