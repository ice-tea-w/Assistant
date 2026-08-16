# Qt 头文件大小写错误导致编译失败

## 现象

```text
fatal error C1083: 无法打开包括文件: "Qpixmap": No such file or directory
```

## 根因

Qt 头文件名严格区分大小写。`#include <Qpixmap>` 应该是 `#include <QPixmap>`（大写 P）。

常见的大小写易错：

| 错误 | 正确 |
|------|------|
| `<Qpixmap>` | `<QPixmap>` |
| `<Qapplication>` | `<QApplication>` |
| `<Qstring>` | `<QString>` |
| `<Qobject>` | `<QObject>` |
| `<Qpainter>` | `<QPainter>` |

## 修复

按 Qt 官方文档或 IDE 补全使用正确的大小写。

## 为什么 VS Code 没提示

- C++ 在 Windows 上文件系统不区分大小写，编辑器打开时不报错
- MSVC/cl.exe 在编译时才解析 include，且区分大小写
- 建议安装 `ms-vscode.cpptools` 插件获得实时错误提示
