# 安全模块 C++（M3）

> 状态（2026-11-26）：**唯一 C++ 主线 = `tools/file_guard_cpp`（v1.0.0）**。
> 主程序通过 `SecurityBridge` 调用 `file_guard.exe --json`；Python 版
> （`tools/file_guard`）只作为验收基准/参考实现，不参与运行时。
> 原 `src/security` 的最小移植（crypto/scan/audit 三件套）已删除，原因见 §6。

## 1. 主线结构

```
tools/file_guard_cpp/
├─ include/file_guard/…     公共头（crypto / vault / scan / audit / backup / watch / service / integrity / zipx / util / config）
├─ src/…                    实现 + CLI（main.cpp）
├─ tests/test_core.cpp      58 条测试
├─ CMakeLists.txt           fg_core 静态库 + file_guard.exe + fg_tests.exe
└─ docs/ARCHITECTURE.md     分层、格式、不变量、失败语义
```

实现语言 C++20，密码学全部走 Windows CNG（bcrypt），DPAPI 走 crypt32，
本地服务用 Winsock2，目录监听用 `ReadDirectoryChangesW`，Office 正文抽取用自研
ZIP + DEFLATE（`zipx`）。**不依赖 Qt / vcpkg / 第三方库。**

## 2. 模块

| 模块 | 职责 |
| --- | --- |
| `crypto` | FGC1 容器：PBKDF2-HMAC-SHA256 + 分块 AES-256-GCM + 80B 头 + AAD + 原子写 |
| `vault` | 保险箱：DEK + DPAPI + 恢复码（Crockford Base32）+ 名称加密清单 + 口令轮换 |
| `scan` | 身份证 mod11 / 银行卡 Luhn / USCC mod31 / 手机号 / 关键词；docx/xlsx/pptx 正文 |
| `audit` | 逐条随机 nonce + HMAC 链 + meta 签名 + 高水位 |
| `backup` | 内容寻址 + 加密对象/索引 + 版本 + diff + purge + gc |
| `watch` | 目录监听 + 空闲自动加密（verify 通过后才删明文） |
| `service` | 回环 HTTP JSON-RPC：token + Host/Origin + 异步 job 队列 |
| `integrity` | 完整性清单：SHA-256 + HMAC |
| `zipx` | 最小 ZIP + 原始 DEFLATE 解压（防 zip 炸弹） |

## 3. 构建与测试

```powershell
# 主构建（根 CMake 会把 file_guard_cpp 作为子工程，并拷贝 file_guard.exe 到 Assistant.exe 旁）
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Debug --target Assistant file_guard fg_tests
ctest -C Debug -R fg_tests

# 也可单独构建（不需要 Qt/vcpkg）
cmake -B build/fg-cpp -S tools/file_guard_cpp -G "Visual Studio 17 2022" -A x64
cmake --build build/fg-cpp --config Debug
build/fg-cpp/Debug/fg_tests.exe tools/file_guard_cpp/tests/data   # 58/58
```

## 4. 与主程序接入

`SecurityBridge`（`include/ui/SecurityBridge.h`）只做展示与触发，所有探针都是
**异步 QProcess + `--json` + 30 s 超时**，不拼 shell：

```
SecurityPage.qml
  └─ security.refresh()/runSelfCheck()/runScan(path)
       └─ SecurityBridge ── QProcess: file_guard.exe <cmd> --json --home <AppConfig>/security
            ├─ version                       → 1.0.0
            ├─ doctor --json                 → {"ok":…,"checks":[…]}
            ├─ log verify --json             → {"ok":…,"records":…,"head":…}
            ├─ backup list --json            → {"items":[…]}
            ├─ vault info --json             → {"files":…,"containers":…,"dpapi":…}
            └─ scan <path> --min-risk medium --json → {"summary":…,"files":[…]}
```

- 找不到 `file_guard.exe` 时降级为只读状态，不影响主程序启动；
- 数据目录固定 `%LOCALAPPDATA%/AssistantTeam/Assistant/security`；
- `file_guard.exe` 的查找顺序：`ASSISTANT_FILE_GUARD` → 主程序同目录 →
  仓库内各构建目录。

## 5. 数据格式

- **FGC1 容器**：80 字节大端头（magic `FGC1` / version / KDF / cipher / iterations /
  chunk_size / salt / nonce_prefix / plaintext_size / header_digest），
  每块 `AES-256-GCM`，AAD = `header || BE32(index) || final`；
  与 `tools/file_guard`（Python）字节级兼容（历史 fixture 已验证双向互解）。
- **审计日志**：语义与 Python v2 相同（`FGL1` + 逐条随机 nonce + HMAC 链 + `next_seq` 高水位），
  但密钥派生参数独立，**不要和 Python 日志混读**；需要迁移时写一次性工具。
- **保险箱**：C++ 原生 `vault.keys`（key=value 密文块）+ `containers/<随机hex>.fgc`
  + 加密 `manifest/`；不承诺读 Python 版保险箱。

## 6. 为什么删掉 `src/security`

`src/security` 是 2026-11-25 先合入的**最小移植**（crypto/scan/audit，34 条测试）：
它把核心三层编进 `Assistant.exe`，但 `SecurityBridge` 仍走 Python，所以那套 C++ 代码
**没有任何 UI 调用点**。同晚合入的 `tools/file_guard_cpp` 是同一份代码的**超集**：

- 三个核心模块与 `src/security` 逐字同源（归一化 diff 只有几十行差异）；
- 多出 vault / backup / watch / service / integrity / zipx / CLI，且 core 更成熟
  （流式 SHA-256、verify 不落临时明文、随机临时文件名）；
- 有独立构建和 58 条测试。

两套并存只会造成“修 bug 改两处、格式漂移”。因此收敛为：

- **C++ 主线**：`tools/file_guard_cpp`；
- **运行时入口**：`file_guard.exe --json`（`SecurityBridge`）；
- **Python**：`tools/file_guard` 退为验收基准/参考实现，不参与构建；
- **`src/security`**：删除（其能力已全部被 `file_guard_cpp` 覆盖）。

## 7. 诚实边界

- `service` 默认只绑回环；公网/远程不在本模块承诺内；
- 审计日志与 Python 版不共享密钥派生参数，历史日志需要迁移工具；
- 保险箱不承诺读 Python 版数据；恢复码泄露 = 保险箱泄露；
- 运行期内存无明文、驱动级阻断、`security.db` / `FileOpGateway` / 策略审批仍未做
  （M3 下一阶段，见 `docs/architecture/四模块完整架构.md` §6）。
