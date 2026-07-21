# Assistant — Windows 桌面助手

一个使用 C++/Qt 构建的 Windows 桌面助手软件，支持系统托盘、全局热键、插件扩展。

## 开发状态

🚧 初期规划阶段 — 详见 [GUIDE.md](GUIDE.md)

## 技术栈

- **语言:** C++20
- **GUI:** Qt 6
- **构建:** CMake + vcpkg
- **测试:** Google Test
- **容器化:** Docker
- **CI/CD:** GitHub Actions

## 快速开始

```bash
# 克隆项目
git clone <repo-url> && cd Assistant

# 安装依赖 (需要 vcpkg)
vcpkg install

# 构建
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build

# 运行
./build/Debug/Assistant.exe
```

## 项目结构

```
Assistant/
├── src/            # 源代码
│   ├── core/       # 核心逻辑
│   ├── ui/         # 界面模块
│   ├── plugins/    # 插件系统
│   └── utils/      # 工具类
├── include/        # 头文件
├── tests/          # 测试
├── resources/      # 资源文件
├── docker/         # Docker 配置
└── docs/           # 文档
```

## 许可证

MIT
