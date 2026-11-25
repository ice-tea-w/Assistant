# 安全模块 C++ 移植说明（M3）

> 目标：把 `tools/file_guard`（Python，约 5400 行）的安全能力搬进主程序（C++20 / Win32 CNG），
> 不引入 Qt/vcpkg 之外的依赖。**本轮完成核心三层并通过跨语言互操作验证**；
> 保险箱/备份/本地服务留下阶段（见 §6）。

## 1. 本轮移植范围

| 模块 | 状态 | 文件 |
| --- | --- | --- |
| **加密容器（FGC1）** | ✅ 完成，**与 Python 字节级兼容** | `include/security/SecurityCrypto.h`、`src/security/SecurityCrypto.cpp` |
| **敏感扫描** | ✅ 完成（身份证 mod 11 / 银行卡 Luhn / USCC mod 31 / 手机号 / 关键词，样本打码） | `include/security/SecurityScan.h`、`src/security/SecurityScan.cpp` |
| **审计日志** | ✅ 完成（逐条随机 nonce + HMAC 链 + meta 高水位；C++ 版格式 v2） | `include/security/SecurityAudit.h`、`src/security/SecurityAudit.cpp` |
| 保险箱（DEK/恢复码/名称加密） | ⏳ 下一阶段（Python 版 v0.6 已有，接口/格式待定） | — |
| 备份 / 监听 / 本地服务 | ⏳ 下一阶段 | — |

实现选型：**Windows CNG（bcrypt.dll）**——AES-256-GCM / PBKDF2-HMAC-SHA256 /
SHA-256 / HMAC / 随机数全部走系统加密提供者，不新增第三方库；
`SecurityBridge` 现阶段仍通过 QProcess 调 Python，后续可切换到 C++ 直调（接口已按门面设计）。

## 2. FGC1 容器兼容性（已双向验证）

格式与 Python 版完全一致：

```
offset size  field
0      4     magic  "FGC1"
4      1     version=1
5      1     kdf_id=1 (PBKDF2-HMAC-SHA256)
6      1     cipher_id=1 (AES-256-GCM)
7      1     flags=0
8      4     iterations（大端）
12     4     chunk_size（大端）
16     16    salt
32     8     nonce_prefix（每块 nonce = prefix || BE32(counter)）
40     8     plaintext_size（大端）
48     32    header_digest = SHA-256(前 48 字节)
80     ...   ciphertext(chunk) || tag(16) 逐块排列
```

每块 AAD = `header(80) || BE32(块号) || 1 字节"是否最后一块"`；
解密先写临时文件、校验通过后原子替换（`MoveFileExW`），失败不留半截明文。

**互操作验证（已跑通）**：

```
Python 生成 python_made.fgc → C++ 解密：明文 SHA-256 = 63243fcd174c29e6…（与 Python 记录一致）
C++ 生成 cpp_made.fgc     → Python verify_file：通过（sha256=63243fcd174c29e6…）
```

## 3. 测试

```powershell
# 独立编译（不依赖 Qt/vcpkg；已验证）
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /std:c++20 /EHsc /utf-8 /I include src\security\SecurityCrypto.cpp `
   src\security\SecurityScan.cpp src\security\SecurityAudit.cpp `
   tests\security_core_tests.cpp /Fe:build\security_core_tests.exe /link bcrypt.lib
.\build\security_core_tests.exe tests\security\data
# → 结果：34/34 通过 ✔
```

覆盖：SHA-256 标准向量、Python 容器解密、C++ 加密往返、错误密码、篡改 1 字节、
头部篡改、审计链校验/改行检出、身份证/Luhn 校验位、样本打码、目录扫描。

fixture 生成（用 Python 原型作参考实现）：

```powershell
.venv\Scripts\python.exe tests\security\make_fixture.py            # 生成
.venv\Scripts\python.exe tests\security\make_fixture.py --check-cpp # 校验 C++ 产物
```

## 4. 接入主程序

- 源文件已放入 `src/security/`，CMake 的 `file(GLOB_RECURSE src/*.cpp)` 会自动带进主目标；
  主目标已补 `bcrypt` 链接；
- 新增 `assistant_security_tests`（纯 C++，加入 ctest，不需要 Qt）：
  `ctest -R assistant_security_tests`；
- `SecurityBridge` 仍走 Python（保证现有「安全中心」页可用）；下一步把它改为优先 `security::crypto`
  直调，Python 仅作为 fallback / 验收基准。

## 5. 审计格式说明（重要）

C++ 版审计日志**语义**与 Python v2 一致（逐条随机 12B nonce、`h_i = HMAC(chain_key, h_{i-1}||nonce||ct)`、
meta 签名 + `next_seq` 高水位、AAD = `"FGL1" || BE64(seq) || nonce`），但**文件格式是独立的 C++ v2**：

```
audit.meta.json  {"version":2,"salt":b64,"iterations":N,"next_seq":N,"head":hex,"mac":hex}
audit.jsonl      {"seq":N,"n":b64(nonce),"h":hex(chain),"d":b64(ciphertext+tag)}
```

与 Python 版不共享密钥派生参数（脚本已有独立的 salt/迭代数），因此**不能直接混读**；
如需迁移历史日志，需要写一次性迁移工具（下一阶段）。

## 6. 下一阶段（按优先级）

1. **保险箱**：DEK + DPAPI 包裹 + 恢复码（Crockford Base32）+ 名称加密清单；
2. **备份**：内容寻址（HMAC 子密钥）+ 加密索引 + 版本回滚 + 冲突保护；
3. `SecurityBridge` 切 C++ 直调（保留 Python fallback）；
4. Python→C++ 审计日志迁移工具；
5. 本地服务（回环 + token）如仍需独立进程再移植。

## 7. 诚实边界

- 本轮只是**核心安全能力**移植，不等于 Python 模块 100% 等价（备份/保险箱/服务未移）；
- `verifyFile` 为计算明文哈希会临时落盘（Python 版是纯流式内存），大文件会多一次磁盘 I/O；
- 审计日志与 Python 版不同格式（见 §5）；
- C++ 侧仍需要主程序构建（vcpkg + Qt6）才能进「安全中心」页；独立测试不需要。
