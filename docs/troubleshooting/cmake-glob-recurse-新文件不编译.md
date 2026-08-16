# file(GLOB_RECURSE) 新增文件后链接报 LNK2019

## 现象

```text
error LNK2019: 无法解析的外部符号 (构造函数/成员函数)
error LNK1120: N 个无法解析的外部命令
```

新增了 `.cpp` 文件，直接 `cmake --build` 编译通过但链接报符号找不到。

## 根因

`file(GLOB_RECURSE SOURCES src/*.cpp)` **只在 CMake 配置阶段（`cmake -B build -S .`）展开**。配置之后新增的文件不会自动被发现，`.cpp` 根本没被编译，所以 `.obj` 不存在，链接阶段报 LNK2019。

## 修复

### 方案 A：重新配置（手动）

每次新增文件后重跑 configure：

```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

### 方案 B：加 CONFIGURE_DEPENDS（自动化，推荐）

```cmake
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS include/*.h)
```

MSBuild 会在 build 时检查文件列表变化，自动触发 reconfigure。代价是每次 build 略慢一点。

### 方案 C：显式列出所有源文件（最稳定）

```cmake
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/core/HotkeyManager.cpp
    src/ui/TrayManager.cpp
    # ...
)
```

不需要 GLOB，每次加文件手动加到 CMakeLists.txt。对大项目更可控，对小项目稍麻烦。

## 区分：链接报错 vs 编译报错

- 如果只有 `.cpp` 没编译 → **LNK2019**（链接阶段）
- 如果 `.cpp` 编译了但头文件声明不匹配 → **LNK2019**（moc 未生成）
- 如果 `.cpp` 内部语法错误 → **C2xxx**（编译阶段）
