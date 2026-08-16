# vcpkg 与编译器匹配

## 核心原则

vcpkg 安装依赖时需要一个 **triplet**（目标平台标识），triplet 决定了编译出的库用什么编译器、链接什么运行时。

## 常用 triplet

| triplet | 编译器 | 产出文件 | 适用场景 |
|---------|--------|---------|---------|
| `x64-windows` | MSVC | `.lib` + `.dll`（动态链接） | Windows 桌面开发（默认） |
| `x64-windows-static` | MSVC | `.lib`（静态链接） | 单文件发布 |
| `x64-mingw-static` | MinGW GCC | `.a`（静态链接） | MSYS2 环境下开发 |

## 常见错误

### 错误：vcpkg 的 triplet 和编译器不匹配

```
undefined reference to `__imp__ZN...'
```

这就是典型的 **MSVC .lib 被 MinGW GCC 使用**，或反过来。

**诊断方法：**
1. 确认编译器：`g++ --version` 还是 `cl`？
2. 确认 triplet：看 `vcpkg install` 用的是什么参数
3. 确认 CMake 传了什么 triplet：`-DVCPKG_TARGET_TRIPLET=xxx`

### 解决方案二选一

| 方案        | 操作                                   | 代价             |
| --------- | ------------------------------------ | -------------- |
| 装 MSVC    | 安装 VS2022 Build Tools                | 免费，安装 ~2GB     |
| 换 triplet | `vcpkg install qt6:x64-mingw-static` | Qt6 重编译 ~60 分钟 |

## 经验教训

- **先装 VS Build Tools，再开始装 vcpkg 依赖** —— 避免 GCC/MSVC 混用
- **大型库（Qt）一定要选对 triplet** —— 装错重来代价很大
- **用 `x64-windows`（MSVC）是最省心的选择** —— Qt 官方和 vcpkg 都优先支持这个 triplet
