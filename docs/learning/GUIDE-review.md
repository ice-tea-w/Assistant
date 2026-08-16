# 《桌面助手软件开发流程指南》评审报告

> 本文档是对 GUIDE.md 的技术评审，包含第三方分析意见及对分析的评价。
> 阅读顺序：先读 GUIDE.md → 再读本文档。

---

## 一、总体评价

GUIDE.md 是一份**规划水平较高**的项目路线图，接近一个中小型商业 C++ 桌面软件的工程化路线，不是单纯写一个 Demo。评分为（按个人开发项目标准）：

| 项目       | 评价      |
| ---------- | --------- |
| 项目架构   | ⭐⭐⭐⭐⭐ |
| 工程规范   | ⭐⭐⭐⭐⭐ |
| 技术选型   | ⭐⭐⭐⭐   |
| 学习曲线   | ⭐⭐⭐     |
| 实际完成可能性 | ⭐⭐⭐     |
| 商业化潜力 | ⭐⭐⭐⭐⭐ |

### 优点

- ✅ 有完整的软件生命周期：需求 → 架构设计 → 编码 → 测试 → CI/CD → 发布
- ✅ 引入了 CMake、vcpkg、Git Flow、Docker、CI/CD、单元测试、插件系统——都是工业项目常见工具

### 核心问题

> 把一个"桌面助手 MVP"规划成了"腾讯级客户端工程"。

对于第一次完整 C++ 软件开发，容易陷入：

```
配置环境 → 解决依赖 → 研究 CMake → 研究 Qt → 研究 Docker
→ 半年过去 → 软件还没打开
```

---

## 二、修订建议

### 2.1 删除 MSYS2，只保留 VS2022 + MSVC

**问题：** 原 GUIDE 同时列出了 MSYS2 和 VS2022，等于同时维护 MSVC 和 MinGW 两套工具链。

**建议：** 删除 MSYS2，只保留以下工具链组合：

```
Visual Studio 2022 Community
    └── MSVC
          ├── CMake
          ├── vcpkg
          └── Qt6
```

这是 Qt 官方对 Windows 平台最推荐的路线。避免 ABI 不兼容、DLL 问题、vcpkg triplet 混乱等问题。

### 2.2 时间线调整

**原计划：**

```
Week 1-2: MVP（托盘+窗口+QML通信）
Week 3-4: 核心（热键+搜索+启动器+配置）
Week 5-6: 扩展（剪贴板+计算器+文件搜索+待办事项）
Week 7-8: 插件系统
Week 9-10: 优化发布
```

**问题：** 对于第一次配置 Qt + CMake + vcpkg + QML 的新手，每个环节都会踩坑。

**修订建议：**

| 阶段    | 周期     | 内容                                    | 里程碑  |
| ------- | -------- | --------------------------------------- | ------- |
| Phase 0 | 1 周     | 环境搭建：VS2022 + Qt6 + CMake + Git + 第一个窗口 | 跑通 Hello Assistant |
| Phase 1 | 3 周     | MVP：系统托盘 + 快捷键 + 窗口显示 + 配置保存 | v0.1.0  |
| Phase 2 | 4 周     | 核心：搜索/命令输入 + 应用启动 + 计算器 + 笔记 | v0.2.0  |
| Phase 3 | 1-2 个月 | 高级功能 + 插件系统                      | v0.5.0  |

### 2.3 插件系统延期

**原计划：** Week 7-8 即引入插件系统。

**问题：** 插件系统需要 ABI 设计、DLL 加载、生命周期管理、接口版本控制。如果核心架构没有稳定，插件接口会一直改。

**修订建议：** 至少等到 v0.3 之后再引入。

### 2.4 Docker 权重降低

**问题：** 原 GUIDE 给了 Docker 整整一个阶段（第五阶段），包含 Dockerfile、docker-compose、多容器编排。

**核心事实：** Docker 不能运行 Windows GUI 程序。对于一个桌面助手，Docker 主要用于 CI（GitHub Actions 编译环境）。

**修订建议：**

```
错误顺序：项目 → Docker → 代码
正确顺序：代码 → 软件运行 → 测试 → Docker CI
```

前期不要在 Docker 上投入大量时间。

---

## 三、缺失模块补充

原 GUIDE 缺少以下对桌面软件至关重要的模块：

### 3.1 自动更新系统

桌面软件的标准需求：检查版本 → 下载更新 → 替换 exe。

可选技术：
- **WinSparkle**（Windows 上类似 Sparkle 的自动更新框架）
- **Squirrel.Windows**

### 3.2 崩溃报告

程序崩溃时生成 dump 文件并上传日志，是定位用户侧问题的唯一手段。

可选技术：
- **Crashpad**（Chromium 的崩溃报告库）
- **CrashRpt**

### 3.3 日志规范

原 GUIDE 选了 spdlog（正确），建议补充日志目录规范：

```
logs/
  └── assistant.log
```

---

## 四、技术栈最终推荐

| 模块   | 推荐             | 备注             |
| ------ | ---------------- | ---------------- |
| 语言   | C++20            |                  |
| IDE    | VS2022           | Community 版即可 |
| UI     | Qt6 QML          | Quick Controls 2 |
| 构建   | CMake 3.20+      |                  |
| 依赖   | vcpkg            | triplet: x64-windows |
| 数据库 | SQLite           |                  |
| JSON   | nlohmann/json    |                  |
| 日志   | spdlog           |                  |
| 测试   | GoogleTest       |                  |
| 版本控制 | GitHub         |                  |
| CI     | GitHub Actions   | windows-latest runner |
| 安装包 | NSIS             | 或 WiX Toolset   |
| 更新   | 后期加入         | WinSparkle       |
| Docker | 仅用于 CI        |                  |

---

## 五、关于 Git Flow 的讨论

原 GUIDE 采用传统 Git Flow（master → develop → feature），评审意见建议简化为 trunk-based（main → feature branch → PR → merge）。

**两种观点都有道理：**

- **简化版（推荐用于纯效率导向）：** 一个人开发时，`develop` 分支是额外成本，直接在 `main` 上开 feature 分支即可。
- **原版（推荐用于学习导向）：** GUIDE.md 的定位明确是"同时学习 Git 版本控制流程"，完整走一遍 Git Flow 的分支模型本身就是学习目标。

取舍取决于项目定位：更看重效率还是更看重学习。

---

## 六、关于目录结构的讨论

**原版：**

```
src/
├── core/
├── ui/
├── plugins/
└── utils/
```

**评审建议版：**

```
src/
├── app/main.cpp
├── core/
├── services/
├── ui/
├── plugins/
└── database/
```

两种方案都合理。原版的 `utils/` 对新手更友好（认知负担低）；评审版的 `services/` 更体现"桌面软件核心是 Service"的设计理念。对于 5-10 人月的项目规模，二分法（core/utils）足够清晰。

---

## 七、GUIDE.md 中特别值得肯定的设计

1. **Conventional Commits 规范**（2.4 节）：在项目一开始就定下提交信息规范，是很多工作多年的程序员都没养成的习惯。
2. **CMakeLists.txt 是可直接运行的模板**：`CMAKE_AUTOMOC`、`CMAKE_AUTORCC`、Qt6 的 find_package 写法均正确，说明经过了真实调研。
3. **vcpkg.json 依赖列表合理**：每个依赖都有明确用途，无冗余。
4. **CI/CD 配置选择了 `windows-latest` runner**：而不是常见的 `ubuntu-latest`，说明考虑了 Windows 桌面软件的特殊性。
5. **附录的效率工具**（VS Code 快捷键 + bash alias）：能实际提升日常开发效率。

---

## 八、执行建议总结

### 删除
- ❌ MSYS2 + VS2022 双工具链
- ❌ 前期 Docker 深入
- ❌ Week 7-8 就引入插件系统

### 调整
- ✅ VS2022 + MSVC + Qt6 + vcpkg 一条线走到底
- ✅ 先做 MVP，插件系统延期到 v0.3+
- ✅ Docker 放到 CI 阶段
- ✅ 补充自动更新和崩溃报告模块
- ✅ 时间线按 Phase 0 → 1 → 2 → 3 重排

### 预期成果

如果按修订后的路线执行：

- **3 个月**可做出一个真正可用的桌面助手
- **6 个月**可做到比较完整的工程级项目

完成后项目水平接近：校级优秀项目、C++ 工程实习作品、简历亮点项目。

---

> **评审结论：** GUIDE.md 方向完全正确，基础已经很好，主要需要降低前期复杂度。修订后成功率将大幅提升。
