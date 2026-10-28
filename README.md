# file_guard · 桌面助手「文件安全模块」Python 参考实现

> 对应《桌面助手 — 文件安全模块设计方案》四大子模块（加密解密 / 敏感检测 / 操作审计 / 安全备份），
> 是一套**能直接跑起来、能接进桌面助手**的最小实现，不是伪代码。

## 1. 快速开始

> 工程已自带 `.venv`（Python 3.12.10 + cryptography 50.0.2），PyCharm 打开即可运行；
> 演示用的合同文件已放在**工程根目录** `合同.docx`（随工程一起拷走即可换机使用）。
> 换机器时再按下面命令重建环境（在工程根目录执行）。
>
> 注意：`.venv` 和数据目录 `~/.file_guard`（密码保险箱/审计/备份）是**跟机器绑定**的——
> 换机后重建虚拟环境、重新 `init` 即可；`合同.docx` 这类演示文件跟着工程目录走。

```bash
# 1) 建虚拟环境（Python 3.10+，实测 3.12 + cryptography 50）
python -m venv .venv
.venv\Scripts\activate          # Windows PowerShell: .venv\Scripts\Activate.ps1
pip install -r requirements.txt

# 2) 初始化（数据目录默认 ~/.file_guard，密码不会明文落盘）
python -m file_guard init

# 3) 一键加密 / 解密（演示文件在工程根目录：合同.docx）
python -m file_guard encrypt "合同.docx"
python -m file_guard decrypt "合同.docx.fgc" --force

# 4) 主动扫描敏感文件（只扫你指定的目录，不做后台全盘扫描）
python -m file_guard scan "合同.docx" --min-risk medium --out report.json

# 5) 审计日志
python -m file_guard log list --limit 20
python -m file_guard log verify          # 校验哈希链有没有被改过

# 6) 增量加密备份 + 误删恢复 / 版本回滚
python -m file_guard backup add "合同.docx"
python -m file_guard backup list "合同.docx"
python -m file_guard backup restore "合同.docx" --version 1

# 7) 作为本地服务跑起来，给桌面助手主程序调用
python -m file_guard serve --port 8765
```

密码来源优先级：`--password` > `--keyfile` > 环境变量 `FG_PASSWORD` > Windows DPAPI 密码保险箱 > 交互输入。

没有装依赖也能先跑 demo 看效果：

```bash
python examples/demo.py
```

## 2. 目录结构

```
file_guard/
├─ file_guard/
│  ├─ crypto.py      # 底层安全层：PBKDF2 + AES-256-GCM 分块容器 + 密钥文件 + DPAPI 保险箱
│  ├─ scan.py        # 业务层：敏感文件检测（身份证/银行卡/手机号/合同/密钥/涉密标记）
│  ├─ audit.py       # 业务层：逐条加密 + HMAC 哈希链审计日志（v2：逐条随机 nonce + meta 签名）
│  ├─ backup.py      # 业务层：内容寻址增量加密备份、版本回滚、完整性校验
│  ├─ watch.py       # 保护目录监听 + 空闲自动加密（删明文前先校验可解密）
│  ├─ integrity.py   # 模块自身哈希清单 + HMAC 签名（防篡改告警）
│  ├─ fsutil.py      # 文件系统边界：目录联结/符号链接检测、Windows ACL 收紧
│  ├─ service.py     # 本地 HTTP JSON-RPC 服务（独立进程，松耦合，回环 + token + Host/Origin 校验）
│  ├─ cli.py         # 命令行 / 右键菜单 / 主程序子进程调用入口
│  └─ config.py      # 目录布局与参数（带类型/范围校验）
├─ tests/            # 115 个 unittest（含 tests/test_security.py 的安全评审回归用例）
├─ examples/demo.py  # 无依赖也能看的完整演示（需要本目录做 sys.path）
├─ scripts/install-context-menu.ps1   # Windows 右键菜单注册脚本（需手动执行）
├─ scripts/run_acceptance.py          # 临时验收脚本：不接主程序，整链路跑一遍并出报告
├─ docs/从设计方案到代码.md            # 逐页对照 + 集成方式 + 关键代码讲解
├─ docs/安全评审与加固.md              # 安全评审报告：问题清单 / 影响 / 修复 / 残余风险
├─ docs/框架临时验证报告.md            # 上次临时验收的结果（52 项，含耗时与人工确认项）
└─ requirements.txt
```

## 3. 设计稿 → 代码对照

| 设计稿 | 代码落点 | 关键实现 |
| --- | --- | --- |
| 文件加密解密（第 6 页） | `crypto.py` | PBKDF2-HMAC-SHA256（600k 次）+ AES-256-GCM；1 MiB 分块流式；每块 AAD 绑定"头 + 块号 + 是否末块" |
| 密码加密 / 密钥文件两种模式 | `crypto.load_keyfile_material`、`cli keygen` | 32 字节随机密钥文件，取 SHA-256 当口令材料 |
| 临时解密 / 超时自动重新加密 | `watch.auto_encrypt_idle` | 目录轮询 + 空闲时间阈值 + 成功后删除明文（可选） |
| 敏感文件检测（第 6 页） | `scan.py` | 正则 + 校验位（身份证 mod 11、银行卡 Luhn）；docx/xlsx/pptx 用 zip+XML 抽文本；风险分高/中/低 |
| 扫描暂停/限速/低优先级 | `scan.ScanControl`、`--throttle`、`--low-priority` | 协作式暂停/停止 + 令牌桶限速 + Windows 进程优先级 |
| 操作审计日志（第 7 页） | `audit.py` | 每条 JSON 用 AES-256-GCM 单独加密、**每条独立随机 nonce**；`h_i = HMAC-SHA256(h_{i-1} ‖ nonce_i ‖ ct_i)` 防篡改/删除/重排；seq 检测删行；meta 带 HMAC 签名与"下一条序号"高水位，检测回滚 |
| 安全备份恢复（第 7 页） | `backup.py` | 内容 SHA-256 去重（增量）；对象名用密钥化 HMAC（不泄露明文哈希）；备份对象就是 FGC1 容器；索引本身也加密；支持版本回滚、误删恢复、`verify --full`、`gc` |
| 五层架构（第 8 页） | 包结构 | UI=CLI/右键菜单，业务层=scan/audit/backup，底层安全层=crypto，文件系统层=全部走用户授权路径 + 系统目录自动跳过 |
| 插件化、崩溃不影响主程序 | `service.py` + CLI 子进程 | 独立进程 + 本地 HTTP + token；主程序只处理 `{"ok":false}`，模块崩溃不影响主程序；`Session.close()` 可随时关模块 |
| 数据存储表（第 9 页） | 全模块 | 明文文件不落服务端、密码只存 PBKDF2 派生密钥、日志和备份都加密、程序不联网 |
| 模块被篡改（第 10 页） | `integrity.py` | `.py` 哈希清单 + 密码派生 HMAC 签名，`integrity check` 告警 |
| 密码忘记（第 10 页） | 设计约束 | **不设后门**：没有密码就无法解密，代码里没有任何"找回"通道 |

## 4. 安全设计要点（对应第 9 页数据表）

1. **密码永不明文落盘**：只保留 PBKDF2 派生密钥；需要免密自动加密时，用 Windows DPAPI（`CryptProtectData`）保存密码密文，绑定当前 Windows 账户。
2. **一次一密**：每个容器随机 16 字节盐、随机 8 字节 nonce 前缀；同一密码加密同一文件两次，密文也不同。
3. **完整性可验证**：GCM tag 校验失败即拒绝解密；头部改动、截断、追加垃圾数据都会被发现。
4. **不落地半截明文**：解密先写 `.part` 临时文件（直接用 `mkstemp` 的 fd，防同目录符号链接替换），校验通过才 `os.replace` 原子替换；失败自动删除。
5. **日志/备份同样加密**：审计日志逐条加密、备份索引逐条加密，连文件名都不以明文出现。
6. **不联网**：整个包没有任何网络请求代码（`service.py` 只监听 127.0.0.1 的本机回环）。
7. **nonce 不复用**：审计日志每条记录独立随机 12 字节 nonce 并绑定进 AAD/哈希链 —— 日志被回滚后继续追加也不会出现 AES-GCM nonce 复用。
8. **边界守得住**：扫描/监听/备份枚举文件时跳过符号链接与 Windows 目录联结，只碰用户授权目录；本地服务拒绝非回环监听、校验 Host/Origin、token 文件 ACL 收紧到当前用户。
9. **删数据前先证明能恢复**：自动加密删明文前先解密校验；备份写对象时校验明文哈希与索引一致。

> 完整的评审结论、20 项问题清单与残余风险见 **[docs/安全评审与加固.md](docs/安全评审与加固.md)**。

## 5. 接入桌面助手的三条路

| 方式 | 适用场景 | 做法 |
| --- | --- | --- |
| 子进程 + 命令行 | 主程序是 Electron / WPF / Tauri / Python，想省事 | 启动 `python -m file_guard encrypt <路径> --json`，解析 stdout JSON |
| 本地 HTTP 服务 | 需要常驻、频繁调用、统一管理密码会话 | `python -m file_guard serve`，`POST /rpc` + `X-FG-Token`（token 在 `~/.file_guard/service.token`） |
| 直接 import | 主程序本身是 Python | `from file_guard import encrypt_file, SensitiveScanner, AuditLog, BackupStore` |

HTTP 调用示例：

```bash
curl -X POST http://127.0.0.1:8765/rpc ^
  -H "Content-Type: application/json" ^
  -H "X-FG-Token: <token>" ^
  -d "{\"method\":\"scan\",\"params\":{\"paths\":[\"C:\\\\code\\\\file_guard\\\\合同.docx\"]}}"
```

服务的安全约束（已内置）：

- 默认只允许监听回环地址，误写 `--host 0.0.0.0` 会直接拒绝启动；确实要局域网访问需显式
  `--allow-remote`（不建议）；
- 校验 `Host`/`Origin`，挡 DNS rebinding 和浏览器跨站调用；
- `~/.file_guard/service.token` 在 Windows 上用 `icacls` 收紧为仅当前用户可读；
- 单连接 30s 超时、并发 16 上限，畸形参数统一返回 400。

> 桌面助手若是 PyQt/tkinter，**推荐直接 import**（同进程），比走 HTTP 少一层本机信任边界；
> 只有需要"模块崩溃不影响主程序"时才用独立服务进程。

## 6. 测试

```bash
python -m unittest discover -s tests -v
# Ran 115 tests ... OK (skipped=1)
```

覆盖：AES-GCM 往返/篡改/截断/错误密码/原子写、日志链篡改与并发追加、扫描规则与误报、备份去重与版本回滚、DPAPI、HTTP 鉴权与 RPC、CLI 端到端，
以及 `tests/test_security.py` 里的 33 条安全回归（nonce 复用、目录联结越界、zip 炸弹、恶意索引路径、回滚/篡改检测、Host/Origin 校验等）。
最后 1 条跳过的是符号链接用例（当前账户未开开发者模式；目录联结用例可正常运行）。

### 6.1 临时验收（不接主程序，确认框架）

正式接桌面助手之前，建议先跑一遍独立验收：它会在系统临时目录建隔离环境（不碰真实 `~/.file_guard`），
按"环境 → 加密容器 → 敏感检测 → 审计日志 → 备份恢复 → 本地服务 → 监听/自校验/保险箱 → 安全加固抽检 → 性能"
把整条链路走一遍，并生成 Markdown + JSON 报告：

```bash
.venv\Scripts\python.exe scripts\run_acceptance.py
# → docs\框架临时验证报告.md / .json（52 项检查，本次实测全部通过，8.5 秒）
```

它验证的是"框架跑得通、防线生效、三条接入路径（子进程 CLI / 直接 import / 本机 HTTP）可用"，
不替代上面的单元测试；报告末尾会列出需要人工确认的项（右键菜单、真实 UI 联调、打包签名、大目录体感）。

## 7. 已知边界与二期扩展

- **"打开"事件**：轮询只能可靠拿到新增/修改/删除；"打开"需由桌面助手在调用文件时主动 `audit.record("open", ...)`，或用 Windows ETW/Shell 扩展（设计稿二期）。
- **PDF** 正文未抽取（可用 `pypdf` 在 `scan._read_office_text` 旁加一个分支）；图片 OCR 同理。
- **日志追加是 O(1)，但全量校验是 O(n)**：几十万条后建议按天归档；`log clear` 已支持归档。
- **服务为同步 RPC**：超大目录扫描建议改异步 job + 轮询（接口已按 method/params 设计，改造成本低）。
- **完整回滚无法本地自证**：攻击者把日志+meta 整体替换成更早的一致快照时，需要 `log head` 外部锚点
  （注册表/另一台设备）+ `log verify --expected-head` 比对；详见安全评审报告第 5 节。
- 二期功能（水印、U 盘管控、防篡改校验）在 `docs/从设计方案到代码.md` 里标了扩展点。

## 8. 环境自检

刚搭好环境或换机器后，先跑：

```bash
python scripts/check_env.py
```

会检查 Python 版本、`.venv`、`cryptography`、各子模块能否导入、演示文件 `合同.docx` 是否存在，
并做一次临时目录内的加密/解密往返，全部 `[OK]` 就说明环境可用。
