# file_guard · 桌面助手安全模块（C++）

**唯一实现语言：C++20**。无 Qt、无 vcpkg、无第三方库：
Windows CNG 做密码学、crypt32 做 DPAPI、Winsock2 做本地服务、
ReadDirectoryChangesW 做目录监听、自研 ZIP+DEFLATE 做 Office 正文抽取。

> 目录：`E:\Microsoft Visual Studio\projects\file_guard`　｜　版本 1.0.0
> 格式为 **C++ 原生格式**，不承担旧实现的数据迁移。

## 模块

| 模块 | 公共头 | 职责 |
| --- | --- | --- |
| `crypto` | `crypto.h` | FGC1 加密容器：PBKDF2-HMAC-SHA256 + 分块 AES-256-GCM + 80B 头 + AAD + 原子写 |
| `vault` | `vault.h` | 保险箱：DEK + DPAPI + 恢复码（Crockford Base32）+ 名称加密清单 + 口令轮换 |
| `scan` | `scan.h` | 敏感检测：身份证 mod11 / 银行卡 Luhn / USCC mod31 / 手机号 / 关键词；docx/xlsx/pptx 正文 |
| `audit` | `audit.h` | 审计：逐条随机 nonce + HMAC 链 + meta 签名 + 高水位 |
| `backup` | `backup.h` | 内容寻址 + 加密对象/索引 + 版本 + diff + purge（索引压缩重写）+ gc |
| `watch` | `watch.h` | 目录监听 + 空闲自动加密（verify 通过后才删明文） |
| `service` | `service.h` | 回环 HTTP JSON-RPC：token + Host/Origin + 异步 job 队列 |
| `integrity` | `integrity.h` | 完整性清单：SHA-256 + HMAC |
| `zipx` | `zipx.h` | 最小 ZIP + 原始 DEFLATE 解压（防 zip 炸弹） |
| 统一入口 | `file_guard.h` | 版本常量 + `fg` 别名 + 全部公共头 |

## 构建

```powershell
$cmake = 'E:\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
cd 'E:\Microsoft Visual Studio\projects\file_guard'
& $cmake -B build-cmake -S . -G "Visual Studio 18 2026" -A x64
& $cmake --build build-cmake --config Release
# 产物：build-cmake\Release\file_guard.exe、fg_tests.exe
```

无 cmake 时用 MSVC 直接编译（已验证）：

```powershell
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /std:c++20 /EHsc /utf-8 /W3 /I include src\*.cpp /Fe:build\file_guard.exe `
   /link bcrypt.lib crypt32.lib ws2_32.lib advapi32.lib shell32.lib
```

## 测试

```powershell
.\build-cmake\Release\fg_tests.exe tests\data     # → 58/58 通过 ✔
cd build-cmake; ctest -C Release                  # 100% passed
```

覆盖：容器加密/解密/篡改/截断、保险箱（恢复码/名称不落盘/口令轮换/mtime）、
备份（版本/冲突/diff/purge 压缩/gc）、审计链（改行检出）、扫描（校验位/打码/docx 正文）、
监听自动加密、DPAPI 往返、完整性清单。

## CLI 速查

```powershell
$env:FG_PASSWORD = "你的密码"        # 或 init 时启用 DPAPI 缓存
$exe = ".\build-cmake\Release\file_guard.exe"

& $exe init --home D:\fg-home --iterations 600000
& $exe encrypt D:\docs\合同.docx --home D:\fg-home
& $exe encrypt D:\docs --dst D:\docs.enc --home D:\fg-home       # 目录
& $exe verify  D:\docs\合同.docx.fgc --home D:\fg-home
& $exe decrypt D:\docs\合同.docx.fgc --dst D:\out.docx --home D:\fg-home
& $exe keygen --dst D:\fg-home\keyfile.bin --home D:\fg-home
& $exe passwd D:\docs\合同.docx.fgc "新口令" --home D:\fg-home
& $exe scan D:\docs --home D:\fg-home
& $exe log record --event encrypt --detail "合同.docx" --home D:\fg-home
& $exe log verify --home D:\fg-home; & $exe log head --home D:\fg-home
& $exe log export --dst D:\audit.jsonl --home D:\fg-home; & $exe log clear --home D:\fg-home
& $exe backup add D:\docs\合同.docx --home D:\fg-home
& $exe backup diff D:\docs\合同.docx --home D:\fg-home
& $exe backup restore D:\docs\合同.docx --version 1 --home D:\fg-home
& $exe backup restore D:\docs\合同.docx --dest-root E:\newbox --strip-prefix C:\Users\a --home D:\fg-home
& $exe backup purge --keep 5 --home D:\fg-home
& $exe backup verify --home D:\fg-home; & $exe backup gc --home D:\fg-home
& $exe vault create --home D:\fg-home                 # 输出恢复码（只显示一次）
& $exe vault add D:\docs\合同.docx --home D:\fg-home
& $exe vault list --home D:\fg-home
& $exe vault extract 1 --dest D:\out --home D:\fg-home
& $exe vault verify --home D:\fg-home
& $exe vault list --recovery-code "XXXX-..." --home D:\fg-home
& $exe vault recovery --home D:\fg-home
& $exe vault passwd "新口令" --home D:\fg-home
& $exe watch D:\下载 --auto-encrypt 60 --home D:\fg-home
& $exe serve --port 8765 --home D:\fg-home             # token: home\service.token
& $exe integrity build D:\app\*.dll --home D:\fg-home
& $exe doctor --home D:\fg-home
```

HTTP：`POST /` + `X-FG-Token`，body `{"method":"ping"}`；
异步任务：`{"method":"job.submit","op":"scan","path":"D:\\docs"}` → `{"method":"job.status","id":"..."}`。

## 质量与性能改进（本轮）

- **容器哈希改为流式计算**：加密/解密时边处理边算 SHA-256，不再二次整读文件；
  `verify` 不再落临时文件（大文件内存占用从 O(文件大小) 降到 O(1)，少一次磁盘写）；
- **审计追加前一致性预检**：日志尾部与 meta 高水位/链头不一致时拒绝追加，
  避免“崩溃后悄悄写坏审计链”；
- **本地服务 JobManager 改为实例私有**：服务退出时 join 工作线程，消除悬垂 `this` 风险；
  任务队列上限 64、`job.submit` 的 op 走白名单（防注入）、连接 30 秒收发超时；
- **ACL 收紧改用 Win32 API**（`SetFileSecurityW` + SDDL），去掉 `std::system(icacls)` 子进程调用；
- **监听修复**：`--auto-backup` 现在是显式开关（此前无脑备份）；文件参数只监听该文件；
  多 watcher 并发写备份索引加锁；过滤自身 `.part-*` / `.tmp` 事件；
- **backup/integrity 改用流式 SHA-256**，大文件不再整读；
- **临时文件随机化**（内存 API / 校验），避免并发/重复调用冲突。

## 安全边界（明确承诺）

**承诺**：静止态机密性（AES-256-GCM + 随机盐/nonce）；篡改/截断/重排可检出；
口令/密钥不落明文；审计链防改行/删行/回滚（meta 高水位）；恢复码作为第二把钥匙；
本地服务只绑回环并要求 token；删除数据前先校验可恢复。

**不承诺**：运行期内存无明文；SSD/日志/备份的物理擦除；防同 Windows 账户的恶意程序；
恢复码丢失且忘记口令时的数据恢复（无后门）。

## 目录

```
include/file_guard/   公共头（file_guard.h 统一入口 + 各模块）
src/                  实现 + main.cpp（CLI）
tests/test_core.cpp   58 项验证；tests/data/ 含 docx fixture
docs/ARCHITECTURE.md  分层、格式、不变量与失败语义
CMakeLists.txt
```
