# 打包与安装包（v0.3.0 起）

> 目标：把 Release 构建整理成一个**可独立运行**的目录，再用 Inno Setup 打成 per-user 安装包。
> 原则：**程序目录与用户数据分离**；Qt 依赖全部相对化；固定 AppId 支持覆盖升级。

## 1. 产物

```
dist/
├─ Assistant-0.3.0/                 # 可直接运行的 staging（约 120 MB）
│  ├─ Assistant.exe
│  ├─ file_guard.exe                # 安全模块（必须与主程序同目录）
│  ├─ Qt6*.dll + 第三方 DLL
│  ├─ sqlite3.dll                   # qsqlite 的底层依赖
│  ├─ qt.conf                       # Prefix = .（不再指向 vcpkg）
│  └─ Qt6/
│     ├─ plugins/                   # platforms/styles/sqldrivers/imageformats/tls/...
│     └─ qml/                       # 只带用到的模块（Controls/Dialogs/QtQml/...）
└─ Assistant-0.3.0-setup.exe        # 安装包（约 32 MB）
```

## 2. 构建

```powershell
# 1) Release 构建
cmake --build build --config Release --target Assistant file_guard

# 2) staging（调用 scripts/deploy.ps1）
cmake --build build --config Release --target deploy

# 3) 安装包（需要 Inno Setup 6；CMake 会自动找 ISCC）
cmake --build build --config Release --target installer
```

单独跑 staging（不经过 CMake）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Config Release
```

`deploy.ps1` 做的事：

- 拷贝 `Assistant.exe`、`file_guard.exe`；
- 从 vcpkg Release 目录拷贝 Qt DLL、`Qt6/plugins`、`Qt6/qml`（只带用到的模块）；
- 用 `dumpbin /dependents` **递归解析依赖**（避免漏 `sqlite3.dll`、插件依赖）；
- 生成相对化 `qt.conf`：`Prefix = .` / `Plugins = Qt6/plugins` / `QmlImports = Qt6/qml`；
- 去掉 `*.pdb`（Qt 插件/qml 里的 PDB 很大，运行时用不到）。

> vcpkg 版 Qt 没有 `windeployqt`，所以用脚本拷贝而不是 windeployqt。

## 3. 安装包行为

| 项 | 设计 |
| --- | --- |
| 安装位置 | `%LOCALAPPDATA%\Programs\Assistant`（per-user，不需要管理员） |
| 数据位置 | `%LOCALAPPDATA%\AssistantTeam\Assistant`（config、5 个 DB、logs、security） |
| 升级 | 固定 `AppId`，直接覆盖安装；数据目录不动 |
| 卸载 | 只删程序目录；数据保留（需要彻底清理时由用户手动删） |
| 快捷方式 | 开始菜单 + 可选桌面 |
| 运行中升级 | `[Code] PrepareToInstall` 先 `taskkill` Assistant/file_guard |

安装包脚本：`installer/Assistant.iss`。可传 `/D` 覆盖版本/源目录/输出名：

```powershell
ISCC.exe /DMyAppVersion=0.3.1 `
         /DMySourceDir=..\dist\Assistant-0.3.1 `
         /DMyOutputDir=..\dist `
         installer\Assistant.iss
```

## 4. 版本与升级

- 版本号唯一来源：`CMakeLists.txt` 的 `project(Assistant VERSION x.y.z)`；
  安装包的 `DisplayVersion` 与服务端 `appVersion` 都从这里来；
- **DB 迁移**：`DatabaseMigrator` 是 `user_version` 前向迁移，安装包不碰 DB；
  升级后新 exe 自己迁移。迁移失败会拒绝启动（不静默重建）；
- 发布说明要写明**不支持降级**（高版本库会被旧 exe 拒绝）；
- 建议升级前自动备份权威库（downloads/security/todo/notes/clipboard），后续可在
  installer 的 `PrepareToInstall` 里加一步复制。

## 5. 验证清单

1. 干净 Windows VM（无 Qt/vcpkg/Python）安装 → 启动 → 自动建 `%LOCALAPPDATA%` 数据；
2. 确认安全中心能显示 `file_guard.exe` 版本（v1.0.0）；
3. 造一点数据（待办/下载/审计），再跑一次新版本安装包 → 数据仍在、DB 自动迁移；
4. 卸载 → 程序目录消失、数据目录仍在；
5. 检查 `qt.conf` 是相对路径、`platforms/qwindows.dll`、`sqldrivers/qsqlite.dll`、`sqlite3.dll` 都在。

## 6. 已知限制

| 限制 | 说明 |
| --- | --- |
| 未签名 | 首次运行可能被 SmartScreen 拦；正式发布前用代码签名证书签 setup 和 exe |
| 中文向导 | Inno 6 per-user 安装不带 `ChineseSimplified.isl`，当前向导英文；补齐语言包即可 |
| 无自动更新 | 先手动覆盖安装；程序目录可整体替换、数据分离，后续接 WinSparkle/自建清单不用重做安装包 |
| Debug 不能发布 | `dist` 只应来自 Release；Debug 的 Qt DLL 带 `d` 后缀且体积大 |
| 体积 | 当前约 32 MB（lzma2）；Qt QML 只带了用到的模块，后续可按需再裁 |

## 7. 故障排查

- **启动白屏/缺 QML 模块**：看 `qt.conf` 的 `QmlImports` 是否指向 `Qt6/qml`，目录是否随包带上；
- **`qsqlite` 加载失败**：`Qt6/plugins/sqldrivers/qsqlite.dll` + `sqlite3.dll` 必须都在；
- **安全中心显示「未找到 C++ 安全模块」**：`file_guard.exe` 必须与 `Assistant.exe` 同目录；
- **安装时注册表/快捷方式被拒**：受限环境（沙箱）会这样；正常桌面不会。测试可用
  `ISCC /DMyPortable=1` 生成不写卸载键的绿色包验证文件布局。
