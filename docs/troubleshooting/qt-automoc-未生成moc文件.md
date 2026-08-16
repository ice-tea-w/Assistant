# AUTOMOC 未生成 moc_*.cpp 导致 qt_metacall 等符号缺失

## 现象

```text
error LNK2001: 无法解析的外部符号 qt_metacall
error LNK2001: 无法解析的外部符号 qt_metacast
error LNK2001: 无法解析的外部符号 metaObject
error LNK2001: 无法解析的外部符号 staticMetaObject
```

头文件中有 `Q_OBJECT` 宏，`set(CMAKE_AUTOMOC ON)` 已开启，但 `moc_<ClassName>.cpp` 没有生成。

## 可能原因

1. **CMake 缓存脏数据** — 之前的 configure 残留导致 AUTOMOC 依赖关系错乱
2. **头文件不在 AUTOMOC 扫描范围** — 头文件路径没有被 `target_include_directories` 覆盖，或不在 `add_executable` 的源文件列表中
3. **Q_OBJECT 所在类没有被 MOC 发现** — 头文件的 include 路径不完整

## 诊断

检查 AUTOMOC 状态：

```powershell
# 查看 AUTOMOC 是否知道这个头文件
grep "ClassName" build/CMakeFiles/Assistant_autogen.dir/AutogenInfo.json

# 检查 moc 文件是否生成
ls build/Assistant_autogen/include_Debug/*/moc_ClassName.cpp
```

## 修复

### 方案 A：清理 AUTOMOC 输出重编（先试这个）

```powershell
Remove-Item -Recurse -Force build/Assistant_autogen
cmake --build build --config Debug
```

### 方案 B：--clean-first

```powershell
cmake --build build --config Debug --clean-first
```

### 方案 C：彻底清理重建（最后手段）

```powershell
Remove-Item -Recurse -Force build
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

## 预防

1. 新增带 `Q_OBJECT` 的文件后，先 configure 再 build
2. 头文件放在 `include/` 下且被 `target_include_directories` 覆盖
3. 也可以用 `#include "moc_ClassName.cpp"` 在 `.cpp` 末尾手动包含（不需要 AUTOMOC），但这破坏常规项目结构

## 相关

- [[cmake-glob-recurse-新文件不编译]] — 新文件没被 GLOB 扫到也有相同链接症状
