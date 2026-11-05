# Python 参考实现（不参与构建）

本目录是 M3 安全模块的**设计参考 + 可跑原型**，来自 `file_guard` 分支，不属于 C++ 工程，也不进 CMake。

## 目录

| 路径 | 内容 |
|---|---|
| `docs/reference/file_guard/` | 设计文档：原项目 README、从设计方案到代码、安全评审与加固、框架临时验证报告 |
| `tools/file_guard/*.py` | 10 个模块（`crypto` / `scan` / `audit` / `backup` / `watch` / `integrity` / `fsutil` / `service` / `cli` / `config`） |
| `tools/file_guard/tests/` | 115 个 unittest（含 `test_security.py` 的 33 条安全评审回归） |
| `tools/file_guard/examples/demo.py` | 无 UI 的端到端演示（临时目录里跑，结束自动清理） |
| `tools/file_guard/scripts/` | 环境自检、临时验收、右键菜单注册、生成合同样例（需要 `python-docx`） |

## 运行

```powershell
pip install -r tools\file_guard\requirements.txt

# CLI（在仓库根目录；tools 是命名空间包）
python -m tools.file_guard --help

# 单测：脚本自动把 tools/ 加进 PYTHONPATH，再按原工程的命令跑
powershell -ExecutionPolicy Bypass -File tools\file_guard\run-tests.ps1

# 演示
$env:PYTHONPATH = "tools"; python tools\file_guard\examples\demo.py
```

实测：Python 3.14.4 + cryptography 49.0.0 下 115 个用例全过（原工程写的是 Python 3.12 + cryptography 50）。

## 边界（别当产品代码用）

- 宿主的假设是 **Python**（主项目是 C++/Qt）；
- 账本落**加密 JSONL**，不是主项目的 `security.db`；
- `scan.py` 会**读正文**做内容匹配，与主项目"敏感发现默认关闭、先做路径/名称规则"的决策冲突。

真正要移植的是四点设计：FGC1 容器头、审计链、DPAPI 密钥、junction 隔离。
详见 `docs/reference/file_guard/从设计方案到代码.md` 与 `docs/devlog/2026-11-05.md`。
