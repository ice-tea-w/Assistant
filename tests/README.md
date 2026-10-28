# tests/ — 插件核心单测

独立的 CMake 工程，**不参与主程序构建**，也不影响 `build/` 里的内容。

## 为什么单独一个工程

根 `CMakeLists.txt` 里已有一段注释掉的 GTest 配置（`L108-117`），但：

1. 把测试目标加进主工程后，每次改测试都会触发主工程 reconfigure；
2. GTest 目前没装，而这批检查（`SemVer` / `PluginManifest` 的纯逻辑）**不需要任何框架**。

所以这里用「独立工程 + 极简断言」：失败计数 + 非零退出码。将来若启用 GTest，把这些
`CHECK(...)` 逐条换成 `EXPECT_TRUE(...)` 即可，断言内容不用重写。

## 构建与运行

```powershell
$cmake = "D:\STUDY\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$proj  = "D:\STUDY\软件工程\文件管理助手"
$build = "D:\Software\Deepseek Harness\_plugin-tests-build"   # 故意放仓库外，别污染 git

& $cmake -S "$proj\tests" -B $build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="D:/STUDY/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows

& $cmake --build $build --config Release

$env:PATH = "D:\STUDY\vcpkg\installed\x64-windows\bin;$env:PATH"
& "$build\Release\assistant_core_tests.exe"
```

`RESULT: PASS` 且退出码 0 即为通过。

## 当前覆盖范围

| 目标 | 覆盖点 |
|---|---|
| `SemVer.parse` | 合法/预发布/通配符拒绝/畸形串/前导零 |
| `SemVer.compare` | 数值比较（`0.9.0 < 0.10.0`）、预发布低于正式版 |
| `SemVer.toString` | 往返一致 |
| `SemVer.satisfies` | 比较符、`^`、`~`、`1.x` / `1.2.x`、精确、`\|\|`、空范围、多余空格 |
| `SemVer.isValidRange` | 明确拒绝 `*` / `latest` / 空范围 / 不完整版本 / 空 OR 分支 |
| `PluginManifest` 合法 | 全部字段与 `contributes` 三个数组 |
| `PluginManifest` 非法 | 非 JSON、`manifestVersion` 错、id 非法、version 非法、`main` 路径逃逸（`..` / 反斜杠 / 绝对路径 / 盘符）、`runtime` 非法、`engines` 语法 |
| `PluginManifest` 贡献点 | 重复 id、`source` 逃逸、缺 title、反斜杠归一化 |
| `checkEngine` | 范围满足/不满足、未声明视为兼容 |
| `PluginRegistry` 路径 | `plugins.json` / 插件目录 / `data/` / `.cache` 等布局 |
| `PluginRegistry` 首次运行 | 目录树与状态文件自动创建，且**不**误报警告 |
| `PluginRegistry` 往返 | 全字段（`origin` / `problem` / `enabled` / `installedAt`）落盘再读回一致 |
| `PluginRegistry` 删除 | 删后重载一致；删不存在的 id 必须失败 |
| `PluginRegistry` 损坏恢复 | 非 JSON → 备份为 `plugins.json.broken-*` + 重建为空 + 有警告 |
| `PluginRegistry` schema 不匹配 | `schemaVersion: 99` 按损坏处理，绝不半解读 |
| `PluginRegistry` **写失败回滚** | 状态路径不可写时，内存必须回滚（`upsertAndSave` 与 `removeAndSave` 都验） |
| `PluginRegistry` 数据目录 | `ensureDataDir` 创建且幂等 |

## 新增模块时怎么加测试

1. 在测试文件里加一个 `void testXxx()`，用 `SECTION("...")` 分组、`CHECK(...)` 断言；
2. 在 `main()` 里调用它；
3. 若被测算的新 `.cpp` 在 `src/core/plugin/` 下，在 `CMakeLists.txt` 的
   `add_executable` 里加一行源文件路径。
