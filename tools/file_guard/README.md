# Python 参考实现（不参与构建）

本目录是 M3 安全模块的**设计参考 + 可跑原型**，来自 `file_guard` 分支，不属于 C++ 工程，也不进 CMake。

- 设计文档在 `docs/reference/file_guard/`
- 运行：`pip install -r requirements.txt`（需要 Python 3.12），`python -m file_guard --help`
- 从仓库根目录跑：`python -m tools.file_guard --help`

**不要**把它当产品代码：宿主的假设是 Python，账本落加密 JSONL 而不是 `security.db`，
且 `scan.py` 会读正文做内容匹配 —— 与主项目"敏感发现默认关闭"的决策冲突。
真正要移植的是四点设计：FGC1 容器头、审计链、DPAPI 密钥、junction 隔离。
详见 `docs/devlog/2026-11-05.md`。
