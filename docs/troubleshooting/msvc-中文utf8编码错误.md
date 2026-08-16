# MSVC C2001 / C4828：中文 UTF-8 编码导致"常量中有换行符"

## 现象

```text
error C2001: 常量中有换行符
warning C4828: 文件包含在偏移 0x2db 处开始的字符，该字符在当前源字符集中无效(代码页 65001)
```

文件中有中文注释或字符串字面量，编译报错。

## 根因

- 手动创建的 `.cpp` 文件保存为 **UTF-8 without BOM**
- MSVC 默认按系统 locale（中文 Windows = GBK / 代码页 936）解析源文件
- 中文的 UTF-8 字节序列（如 `\xe6\x98\xbe`）在 GBK 下被误解析成非法字符或换行符

## 修复

### 方案 A：CMakeLists.txt 加 `/utf-8`（本项目采用）

```cmake
if(MSVC)
    add_compile_options("/utf-8")
endif()
```

强制 MSVC 将源文件和执行字符集都设为 UTF-8。

### 方案 B：文件存为 UTF-8 with BOM

VS Code 右下角点击编码 → "Save with Encoding" → "UTF-8 with BOM"。

BOM (`\xEF\xBB\xBF`) 告诉 MSVC 这是 UTF-8 文件，自动切换解析方式。

### 方案 C：回避中文字面量

字符串用英文，或使用 `QString::fromUtf8("\xe6\x98\xbe\xe7\xa4\xba")` 十六进制转义。

## 相关

- [[MSVC-UTF-8-source-charset]]
- Phase 1 TrayManager 踩坑，详见 `docs/devlog/2026-07-29.md`
