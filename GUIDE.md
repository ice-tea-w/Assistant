# 桌面助手软件开发流程指南

## 项目概述

使用 C++ 构建一款 Windows 桌面助手软件，同时学习 Git 版本控制、Docker 容器化等现代软件开发流程。

---

## 第一阶段：环境搭建与工具链

### 1.1 开发工具安装

| 工具 | 用途 | 安装方式 |
|------|------|----------|
| **MSYS2** | Windows 上的类 Unix 编译环境 | `winget install msys2` |
| **CMake** | 跨平台构建系统 | `winget install Kitware.CMake` |
| **Git** | 版本控制（已安装） | `winget install Git.Git` |
| **Visual Studio Code** | 代码编辑器 | `winget install Microsoft.VisualStudioCode` |
| **Docker Desktop** | 容器化 | `winget install Docker.DockerDesktop` |
| **vcpkg** | C++ 包管理器 | `git clone https://github.com/Microsoft/vcpkg.git` |

### 1.2 推荐 VS Code 插件

```
ext install ms-vscode.cpptools          # C++ 智能提示
ext install ms-vscode.cmake-tools       # CMake 集成
ext install twxs.cmake                  # CMake 语法高亮
ext install eamodio.gitlens             # Git 增强
ext install ms-azuretools.vscode-docker # Docker 支持
```

### 1.3 初始化项目结构

```bash
# 创建目录骨架
mkdir -p Assistant/src/core        # 核心逻辑
mkdir -p Assistant/src/ui          # 界面模块
mkdir -p Assistant/src/plugins     # 插件系统
mkdir -p Assistant/src/utils       # 工具类
mkdir -p Assistant/include         # 头文件
mkdir -p Assistant/tests           # 单元测试
mkdir -p Assistant/resources       # 资源文件
mkdir -p Assistant/docs            # 文档
mkdir -p Assistant/scripts         # 构建/部署脚本
mkdir -p Assistant/docker          # Docker 配置
mkdir -p Assistant/.github/workflows # CI/CD 配置
```

---

## 第二阶段：Git 版本控制入门

### 2.1 Git 基础概念

```
工作区 (Working Directory)    ← 你编辑代码的地方
    ↓ git add
暂存区 (Staging Area)         ← 准备提交的改动
    ↓ git commit
本地仓库 (Local Repository)   ← 本地的版本历史
    ↓ git push
远程仓库 (Remote Repository)  ← GitHub / GitLab 等
```

### 2.2 常用 Git 命令速查

```bash
# === 配置 ===
git config --global user.name "ice_tea"
git config --global user.email "your@email.com"

# === 日常开发流程 ===
git status                    # 查看当前状态
git add <file>                # 将文件加入暂存区
git add -A                    # 暂存所有改动
git commit -m "描述你的改动"   # 提交到本地仓库
git push origin master        # 推送到远程

# === 分支操作 ===
git branch                    # 查看所有分支
git branch feature/xxx        # 创建新分支
git switch feature/xxx        # 切换到分支
git switch -c feature/xxx     # 创建并切换
git merge feature/xxx         # 合并分支到当前分支
git branch -d feature/xxx     # 删除分支

# === 查看历史 ===
git log --oneline --graph     # 图形化查看历史
git diff                      # 查看未暂存的改动
git diff --staged             # 查看已暂存的改动
git show <commit-hash>        # 查看某次提交详情

# === 撤销操作 ===
git restore <file>            # 撤销工作区改动
git restore --staged <file>   # 取消暂存
git reset --soft HEAD~1       # 撤销最近一次 commit（保留改动）
git revert <commit-hash>      # 安全地撤销某次提交

# === 远程操作 ===
git remote add origin <url>   # 关联远程仓库
git fetch origin              # 获取远程更新（不合并）
git pull origin master        # 拉取并合并远程更新
```

### 2.3 Git 分支策略（推荐 Git Flow 简化版）

```
master (main)     ← 稳定发布版本
  ├── develop     ← 日常开发主线
  │   ├── feature/xxx  ← 功能开发（从 develop 分出）
  │   ├── fix/xxx      ← Bug 修复（从 develop 分出）
  │   └── refactor/xxx ← 代码重构
  └── release/x.x  ← 发布准备分支
```

### 2.4 提交信息规范

采用 Conventional Commits 规范：

```
<type>(<scope>): <描述>

类型(type):
  feat     - 新功能
  fix      - Bug 修复
  docs     - 文档更新
  style    - 代码格式调整（不影响功能）
  refactor - 重构（既不修复 bug 也不添加功能）
  test     - 测试相关
  chore    - 构建/工具链相关
  perf     - 性能优化

示例:
  feat(core): 添加全局快捷键监听模块
  fix(tray): 修复托盘图标点击无响应的问题
  docs(readme): 更新编译环境配置说明
```

---

## 第三阶段：C++ 项目架构设计

### 3.1 技术选型

| 模块 | 技术方案 | 说明 |
|------|----------|------|
| GUI 框架 | **Qt 6** | 跨平台、成熟的 C++ GUI 框架 |
| 构建系统 | **CMake 3.20+** | 现代 C++ 标准构建工具 |
| 包管理 | **vcpkg** | 微软官方的 C++ 包管理器 |
| JSON 解析 | **nlohmann/json** | 轻量级 JSON 库 |
| HTTP 客户端 | **cpp-httplib** | 轻量级 HTTP 库 |
| 数据库 | **SQLite3** | 嵌入式数据库 |
| 测试框架 | **Google Test** | C++ 单元测试框架 |
| 日志 | **spdlog** | 高性能日志库 |
| 热键 | **Windows API** | 系统级全局热键 |

### 3.2 核心功能模块

```
桌面助手 (Desktop Assistant)
│
├── 系统托盘 (System Tray)
│   ├── 托盘图标管理
│   ├── 右键菜单
│   └── 气泡通知
│
├── 全局热键 (Global Hotkeys)
│   ├── 呼出/隐藏主窗口
│   ├── 截图快捷键
│   └── 自定义热键绑定
│
├── 主界面 (Main Window)
│   ├── 搜索/命令输入框
│   ├── 结果展示列表
│   └── 设置面板
│
├── 插件系统 (Plugin System)
│   ├── 插件接口定义
│   ├── 动态加载 (.dll)
│   └── 插件管理
│
├── 功能模块 (Features)
│   ├── 剪贴板管理
│   ├── 快速启动应用
│   ├── 文件搜索
│   ├── 计算器
│   ├── 天气查询
│   ├── 待办事项
│   └── 笔记速记
│
└── 基础设施 (Infrastructure)
    ├── 配置管理 (JSON)
    ├── 本地数据库 (SQLite)
    ├── 网络请求 (HTTP)
    └── 日志系统
```

### 3.3 CMakeLists.txt 模板

```cmake
cmake_minimum_required(VERSION 3.20)
project(Assistant VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)      # Qt MOC 自动处理
set(CMAKE_AUTORCC ON)      # Qt 资源文件自动处理
set(CMAKE_AUTOUIC ON)      # Qt UI 文件自动处理

# 查找 Qt6
find_package(Qt6 REQUIRED COMPONENTS
    Core Widgets Network
)

# 查找其他依赖
find_package(SQLite3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

# 源文件
file(GLOB_RECURSE SOURCES src/*.cpp)
file(GLOB_RECURSE HEADERS include/*.h)

# 主程序
add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
    resources/resources.qrc
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    SQLite::SQLite3
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# 测试
enable_testing()
add_executable(assistant_tests
    tests/test_main.cpp
    tests/test_config.cpp
    # ... 更多测试文件
)
target_link_libraries(assistant_tests PRIVATE
    GTest::gtest_main
    # 链接需要测试的库
)
include(GoogleTest)
gtest_discover_tests(assistant_tests)
```

### 3.4 vcpkg 依赖管理

```json
// vcpkg.json
{
  "name": "assistant",
  "version": "0.1.0",
  "dependencies": [
    "qt6",
    "sqlite3",
    "spdlog",
    "nlohmann-json",
    "cpp-httplib",
    "gtest",
    "clip"
  ]
}
```

---

## 第四阶段：分步开发计划

### Phase 1 — 最小可行原型（MVP）  [Week 1-2]

**目标：** 跑通编译→运行→打包全流程

- [ ] 搭建 CMake + vcpkg 构建环境
- [ ] 创建一个系统托盘图标
- [ ] 点击托盘图标弹出简单窗口
- [ ] 实现 Hello World 级别的功能

**Git 操作实践：**
```bash
git switch -c feature/mvp-tray
# ... 编写代码 ...
git add -A
git commit -m "feat(ui): 实现系统托盘图标和基础窗口"
git push origin feature/mvp-tray
# 在 GitHub 上创建 Pull Request → 合并到 develop
```

### Phase 2 — 核心功能 [Week 3-4]

**目标：** 实现助手的基础能力

- [ ] 全局热键注册（如 Alt+Space 呼出窗口）
- [ ] 搜索框 + 命令解析器
- [ ] 应用快速启动功能
- [ ] 配置文件读写

**Git 操作实践：**
```bash
git switch -c feature/core-commands
git switch -c feature/global-hotkey
# 两个功能并行开发在不同的分支上
```

### Phase 3 — 扩展功能 [Week 5-6]

**目标：** 添加实用的日常功能

- [ ] 剪贴板历史管理
- [ ] 简易计算器
- [ ] 本地文件搜索（基于文件名索引）
- [ ] 待办事项列表（SQLite 存储）

### Phase 4 — 插件系统 [Week 7-8]

**目标：** 设计可扩展的插件架构

- [ ] 定义插件接口（纯虚基类）
- [ ] 动态库加载机制
- [ ] 开发 2-3 个示例插件
- [ ] 插件配置与管理 UI

### Phase 5 — 优化与发布 [Week 9-10]

**目标：** 打磨产品、准备发布

- [ ] 性能优化（启动速度、内存占用）
- [ ] 异常处理与日志完善
- [ ] 自动更新机制
- [ ] 安装包制作（WiX Toolset / NSIS）
- [ ] 编写用户文档

---

## 第五阶段：Docker 容器化

### 5.1 Docker 在 C++ 项目中的角色

对于 C++ 桌面应用，Docker 主要用于：

| 场景 | 说明 |
|------|------|
| **统一编译环境** | 确保所有开发者使用相同的编译器版本和依赖 |
| **CI/CD 构建** | 在 GitHub Actions 等 CI 中自动化构建和测试 |
| **交叉编译** | 在 Linux 容器中交叉编译 Windows 程序 |
| **服务端组件** | 如果助手有后端服务，可以容器化部署 |

### 5.2 Docker 常用命令速查

```bash
# === 镜像操作 ===
docker images                  # 列出所有本地镜像
docker pull <image>:<tag>      # 拉取镜像
docker build -t name:tag .     # 从 Dockerfile 构建镜像
docker rmi <image>             # 删除镜像

# === 容器操作 ===
docker run -it <image> bash    # 交互式运行容器
docker run -d <image>          # 后台运行
docker ps                      # 查看运行中的容器
docker ps -a                   # 查看所有容器（包括停止的）
docker stop <container>        # 停止容器
docker rm <container>          # 删除容器
docker exec -it <container> bash  # 进入运行中的容器

# === 数据与网络 ===
docker volume ls               # 列出数据卷
docker volume create <name>    # 创建数据卷
docker network ls              # 列出网络

# === Docker Compose ===
docker compose up -d           # 启动多容器应用
docker compose down            # 停止并删除
docker compose logs -f         # 查看日志
docker compose ps              # 查看服务状态
```

### 5.3 编译环境 Dockerfile

```dockerfile
# docker/Dockerfile.build
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 安装编译工具链
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    curl \
    zip \
    pkg-config \
    # Qt6 开发库
    qt6-base-dev \
    qt6-base-dev-tools \
    # 其他依赖
    libsqlite3-dev \
    libspdlog-dev \
    nlohmann-json3-dev \
    libgtest-dev \
    # 交叉编译 Windows（可选）
    mingw-w64 \
    && rm -rf /var/lib/apt/lists/*

# 安装 vcpkg
RUN git clone https://github.com/Microsoft/vcpkg.git /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh

ENV VCPKG_ROOT=/opt/vcpkg
ENV CMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake

WORKDIR /workspace
```

### 5.4 CI/CD Docker Compose

```yaml
# docker/docker-compose.yml
version: '3.8'

services:
  build:
    build:
      context: ..
      dockerfile: docker/Dockerfile.build
    volumes:
      - ..:/workspace
    working_dir: /workspace
    command: |
      bash -c "
        cmake -B build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake
        cmake --build build
        cd build && ctest --output-on-failure
      "

  # 如果之后需要后端服务
  # backend:
  #   build:
  #     context: ..
  #     dockerfile: docker/Dockerfile.backend
  #   ports:
  #     - "8080:8080"
```

### 5.5 日常 Docker 使用流程

```bash
# 1. 构建编译镜像
docker build -t assistant-build -f docker/Dockerfile.build .

# 2. 在容器中编译项目
docker run --rm -v ${PWD}:/workspace assistant-build \
    bash -c "cmake -B build && cmake --build build"

# 3. 在容器中运行测试
docker run --rm -v ${PWD}:/workspace assistant-build \
    bash -c "cd build && ctest --output-on-failure"

# 4. 一键构建+测试（使用 docker compose）
cd docker && docker compose run --rm build
```

---

## 第六阶段：CI/CD 自动化

### 6.1 GitHub Actions 配置

```yaml
# .github/workflows/build.yml
name: Build and Test

on:
  push:
    branches: [master, develop]
  pull_request:
    branches: [master, develop]

jobs:
  build:
    runs-on: windows-latest

    steps:
      - uses: actions/checkout@v4

      - name: Setup vcpkg
        run: |
          git clone https://github.com/Microsoft/vcpkg.git C:/vcpkg
          C:/vcpkg/bootstrap-vcpkg.bat

      - name: Configure and Build
        run: |
          cmake -B build -S . \
            -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
          cmake --build build --config Release

      - name: Run Tests
        run: |
          cd build
          ctest --output-on-failure -C Release

      - name: Package
        run: |
          cmake --install build --config Release --prefix install
```

### 6.2 版本标签自动化

```bash
# 打版本标签
git tag -a v0.1.0 -m "首个 MVP 版本"
git push origin v0.1.0

# 查看所有标签
git tag -l

# 切回到某个标签
git checkout v0.1.0
```

---

## 第七阶段：学习路线图

### 每周学习计划（10 周）

```
Week 1:  环境搭建 + Git 基础 (init, add, commit, push, pull)
Week 2:  CMake 入门 + Qt 基础窗口
Week 3:  Git 分支与合并 + 系统托盘开发
Week 4:  Docker 基础 (Dockerfile, build, run)
Week 5:  C++ 设计模式 + 插件接口设计
Week 6:  SQLite 数据持久化
Week 7:  Docker Compose + 多容器编排
Week 8:  Git 高级操作 (rebase, cherry-pick, stash)
Week 9:  CI/CD + GitHub Actions + 自动化测试
Week 10: 打包发布 + 文档撰写 + 项目复盘
```

### 推荐学习资源

| 主题 | 资源 |
|------|------|
| Git | [Pro Git 中文版](https://git-scm.com/book/zh/v2)、[Learn Git Branching](https://learngitbranching.js.org/) |
| CMake | [Modern CMake](https://cliutils.gitlab.io/modern-cmake/)、[CMake 官方教程](https://cmake.org/cmake/help/latest/guide/tutorial/) |
| Docker | [Docker 从入门到实践](https://yeasy.gitbook.io/docker_practice/)、[Play with Docker](https://labs.play-with-docker.com/) |
| Qt 6 | [Qt 官方文档](https://doc.qt.io/qt-6/)、[Qt 示例](https://doc.qt.io/qt-6/qtexamplesandtutorials.html) |
| C++ | [CppReference](https://zh.cppreference.com/)、[C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/) |
| 设计模式 | 《Head First 设计模式》、[refactoring.guru](https://refactoring.guru/design-patterns) |

---

## 附录：快捷键与效率工具

### VS Code 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+Shift+P` | 命令面板 |
| `Ctrl+P` | 快速打开文件 |
| `Ctrl+Shift+F` | 全局搜索 |
| `Ctrl+`` | 打开终端 |
| `Ctrl+B` | 切换侧栏 |
| `F5` | 调试运行 |
| `F9` | 设置断点 |
| `Ctrl+K Ctrl+C` | 注释/取消注释 |

### 常用终端别名（添加到 `~/.bashrc`）

```bash
alias gs='git status'
alias ga='git add -A'
alias gc='git commit -m'
alias gp='git push origin $(git branch --show-current)'
alias gl='git log --oneline --graph -15'
alias gsw='git switch'
alias gswc='git switch -c'
alias dc='docker compose'
alias dcb='docker compose build'
alias dcu='docker compose up -d'
alias dcd='docker compose down'
```

---

> **下一步：** 按照第一阶段开始搭建环境，每完成一个 Phase 就打一个 Git Tag，并提交代码到 GitHub。

> 这篇文章会随着项目进展持续更新。每当学到新东西，就补充到对应章节中。
