# 运行效果截图与原始输出

| 文件 | 内容 |
| --- | --- |
| `01-验收原始输出.txt` | `scripts/run_acceptance.py` 的完整真实 stdout（原版，59 行） |
| `01-验收检查-A至D.png` | 上面文本的渲染图（A~D：环境 / 加密 / 检测 / 审计） |
| `02-验收检查-E至H与结果.png` | 渲染图（E~H：备份 / 服务 / 自校验 / 性能 + 最终结果） |
| `03-CLI实跑原始输出.txt` | CLI 实跑转录，带 `$` 提示符（加密/校验/扫描/日志/备份，46 行） |
| `03-CLI实跑.png` | 上面转录的渲染图 |

**图片来源**：不是截图工具抓屏，而是**抓真实 stdout 后按终端配色渲染**（NSimSun 等宽字体、
2× 超采样再降采样）。文字内容与原始输出完全一致，可直接对照 `.txt` 校验，不存在"美化过的假结果"。

## 重新生成

```bash
# 1) 抓真实输出（会完整跑一遍临时验收，约 10 秒）
.venv\Scripts\python.exe scripts\_capture_run_output.py

# 2) 渲染 PNG（需要 Pillow；Pillow 不是 file_guard 的运行依赖）
.venv\Scripts\python.exe scripts\_render_run_screenshot.py
#   若 venv 里没装 Pillow，可用 DSH 自带 Python：
#   C:\Users\31285\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe scripts\_render_run_screenshot.py
```

## 本次环境与结果

- Python 3.12.10 / cryptography 50.0.2 / Windows（win32）
- 临时验收 52 项：**通过 52、失败 0、跳过 0**，总耗时约 10 秒
- 详细报告见 [../框架临时验证报告.md](../框架临时验证报告.md)
