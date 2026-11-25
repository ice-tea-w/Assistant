# Python 安全模块（M3 参考实现 + 可运行原型）

本目录是**安全模块的 Python 实现**，由主程序通过 `SecurityBridge`（QProcess + `--json`）调用，
在「安全中心」页展示审计链、备份库、保险箱、敏感扫描。**不参与 CMake 构建**。

## 组成

| 文件 | 职责 |
| --- | --- |
| `crypto.py` | FGC1 加密容器：PBKDF2 + 分块 AES-256-GCM + 原子写 + keyfile + DPAPI |
| `vault.py` | 保险箱：DEK 模型 + 恢复码 + 名称加密 + 口令轮换（v0.6） |
| `scan.py` | 敏感文件检测（身份证/银行卡/统一社会信用代码/手机号/合同/密钥） |
| `audit.py` | 审计日志：逐条加密 + 随机 nonce + HMAC 链 + 高水位 |
| `backup.py` | 内容寻址增量加密备份 + 版本回滚 + 冲突保护 + diff |
| `watch.py` | 目录监听：改动自动备份 / 空闲自动加密（删明文前先校验） |
| `fsutil.py` | 重解析点（junction）检测、云盘占位识别、ACL 收紧 |
| `service.py` | 本地回环 HTTP JSON-RPC（token + Host/Origin + 异步 Job） |
| `integrity.py` | 模块自校验清单（哈希 + HMAC 签名） |
| `cli.py` / `config.py` / `errors.py` | 命令行、配置、退出码 |

## 运行（仓库根目录）

```powershell
# 依赖（建议独立 venv；主程序会自动探测 <repo>/.venv 或 tools/file_guard/.venv）
pip install -r tools\file_guard\requirements.txt

# 命令行：-m file_guard 需要 tools/ 在 PYTHONPATH 里
$env:PYTHONPATH = "$PWD\tools"
python -m file_guard --version
python -m file_guard init --home "$env:LOCALAPPDATA\AssistantTeam\Assistant\security"
python -m file_guard scan --help
```

主程序侧解释器探测顺序：`ASSISTANT_PYTHON` → `<repo>/.venv/Scripts/python.exe` →
`tools/file_guard/.venv/Scripts/python.exe` → `PATH` 中的 `python`。

## 测试

```powershell
$env:PYTHONPATH = "$PWD\tools"
python -m unittest discover -s tools\file_guard\tests -q    # 160 条
python tools\file_guard\scripts\run_acceptance.py           # 52 项端到端验收
```

## 与主程序的边界（M3 下一阶段）

已接：`SecurityBridge` + 「安全中心」页（四张状态卡 + 体检/扫描/打开报告）。
未接：`FileOpGateway` 受控操作网关、`IFilePolicy` 策略与审批、`security.db` 权威库
（设计见 `docs/architecture/四模块完整架构.md` §6、`docs/architecture/文件系统四模块-关联与方案.md` §4.3）。
