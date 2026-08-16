# VS2022 Build Tools 安装

## 背景

vcpkg 默认安装 Qt6 使用 `x64-windows` triplet，产出的是 **MSVC 版本** 的 `.lib` 文件。
MSYS2 自带的编译器是 **MinGW GCC**，两套 ABI 不兼容，导致链接时全部 `undefined reference`。

```
C:/msys64/ucrt64/bin/c++.exe          ← MinGW GCC
C:/vcpkg/installed/x64-windows/lib/   ← MSVC .lib 文件
→ ABI 不匹配，链接失败
```

## 解决方案

装 VS2022 Build Tools（不要完整 VS IDE），用 MSVC 编译器匹配已有的 Qt6。

### 安装步骤

```powershell
# 1. 下载安装程序
curl -L -o "$env:TEMP\vs_buildtools.exe" "https://aka.ms/vs/17/release/vs_buildtools.exe"

# 2. 静默安装 C++ 桌面开发工作负载
& "$env:TEMP\vs_buildtools.exe" --quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended
```

### 编译步骤

在 **"x64 Native Tools Command Prompt for VS 2022"**（开始菜单搜索）中：

```cmd
cd /d E:\Assistant
rmdir /s /q build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -G "Ninja"
cmake --build build
```

## 备选方案

如果不想装 VS，则需用 `x64-mingw-static` triplet 重装 Qt6：

```powershell
cd C:\vcpkg
.\vcpkg install qt6:x64-mingw-static
```

然后在 MSYS2 UCRT64 中编译。但 Qt6 编译时间长（30-60 分钟）。
