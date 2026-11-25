# 安全模块架构（C++）

> 实现语言：C++20。格式为 C++ 原生格式；不承担旧实现的数据迁移。
> 本文回答：分层、格式、不变量、失败语义。

## 1. 分层

```
CLI (src/main.cpp) ──► 业务模块（vault/scan/audit/backup/watch/service/integrity）
                                  │
                                  ├─► crypto（FGC1 容器 + CNG 原语）
                                  ├─► util（编码/文件/DPAPI/ACL/UTF-8）
                                  └─► zipx（ZIP + inflate，供 scan 抽取 Office 正文）
```

规则：业务层不直接调 BCrypt；底层不感知业务语义；所有失败抛 `SecurityError`，
CLI 映射为固定退出码（1 参数 / 3 口令 / 4 容器损坏 / 5 完整性 / 6 IO / 7 平台）。

## 2. 加密容器 FGC1（crypto）

80 字节大端头：

| 偏移 | 字段 |
| --- | --- |
| 0–3 | magic `FGC1` |
| 4 | version = 1 |
| 5 | kdf_id = 1（PBKDF2-HMAC-SHA256） |
| 6 | cipher_id = 1（AES-256-GCM） |
| 7 | flags = 0 |
| 8–11 | iterations（大端） |
| 12–15 | chunk_size（大端） |
| 16–31 | salt(16) |
| 32–39 | nonce_prefix(8) |
| 40–47 | plaintext_size（大端） |
| 48–79 | SHA-256(前 48 字节) |

每块：`AES-256-GCM(key, nonce = prefix || BE32(index), AAD = header || BE32(index) || final)`，
密文后接 16 字节 tag。解密校验头部摘要、逐块 tag、尾部多余数据；写盘先临时文件再
`MoveFileExW` 原子替换，失败删临时文件（不留半截明文）。

不变量：
1. 同一次加密每个块 nonce 不同（前缀随机 + 块号）；
2. 头/块序/末块标记都进 AAD，改任何一处都会认证失败；
3. 明文长度由头部声明，尾部多余数据直接拒绝。

## 3. 保险箱（vault）

```
<dir>/vault.keys        key=value：迭代数、口令盐、口令包裹的 DEK、恢复码盐与包裹、
                        恢复码密文（DEK 加密，用于重展示）、DPAPI 包裹的 DEK
<dir>/containers/<hex>.fgc   文件容器（容器名随机，不泄露原文件名）
<dir>/manifest/               名称清单（复用审计链：逐条加密 + HMAC 链）
```

- DEK（32B 随机）是唯一内容密钥；口令/恢复码/DPAPI 各自包裹 DEK；
- 改口令只重包 DEK（容器与清单不重加密）；
- 添加文件记录 name/size/sha256/mtime/container；取出时还原原文件名与 mtime；
- 清单篡改、容器缺失/被改由 `vault verify` 检出。

## 4. 审计日志（audit）

```
audit.meta.json  {"version":2,"salt":b64,"iterations":N,"next_seq":N,"head":hex,"mac":hex}
audit.jsonl      {"seq":N,"n":b64(随机12B nonce),"h":hex(链哈希),"d":b64(密文+tag)}
```

- `ct = AES-GCM(key, nonce, AAD = "FGL1" || BE64(seq) || nonce)`
- `h_i = HMAC(chain_key, h_{i-1} || nonce_i || ct_i)`，`h_0` 由 genesis label + salt 派生；
- meta MAC 防只改 meta；`next_seq` 高水位防截断/回滚；
- `log clear` 归档旧日志并换新盐起点。

## 5. 备份（backup）

```
backups/objects/<2>/<name>.fgc   对象（内容地址 = HMAC(子密钥, SHA-256(明文))）
backups/index/                   加密索引（审计链；每条 = 一次备份事件）
```

- 版本号 = 该路径已有最大版本 + 1；
- 冲突保护：目标当前内容不在任何已备份版本中时拒绝覆盖；`--force` 覆盖前自动快照；
- `diff`：解密两版到临时文件，文本给 unified diff，二进制给摘要；
- `purge --keep N`：保留每组最近 N 版，然后**重写索引目录**（新目录写完再原子换目录），
  `gc` 删除未引用对象——不依赖墓碑记录，索引不随时间无限增长。

## 6. 敏感扫描（scan + zipx）

- 规则：身份证（mod 11）、银行卡（Luhn）、统一社会信用代码（mod 31）、手机号、关键词；
- 样本一律打码；跳过重解析点、二进制、云盘占位文件（避免触发下载）；
- Office：自研 inflate 解 ZIP，抽取 `word/` `xl/` `ppt/slides/` 下的 XML，去标签后扫描；
  上限：单条 4 MB、总 8 MB、最多 50 条（防 zip 炸弹）。

## 7. 监听（watch）

- `ReadDirectoryChangesW` 阻塞线程 → 事件退出，过滤 `.fgc`/隐藏/临时文件；
- `--auto-encrypt N`：空闲超过 N 秒的文件加密为 `<file>.fgc`，**解密校验哈希一致后才删除明文**。

## 8. 本地服务（service）

- 只绑 `127.0.0.1`（`--allow-remote` 才放开）；`Host` 仅允许回环名；带 `Origin` 一律拒绝；
- 强制 `X-FG-Token`（token 文件 ACL 收紧）；请求体上限 1 MB；同步方法 + 异步 job 队列；
- `job.submit/status/list/cancel`：单工作线程串行执行，状态 queued/running/done/error/cancelled。

## 9. 完整性（integrity）

清单文本：每行 `<sha256hex>  <path>`，最后一行 `mac=<HMAC(派生密钥, 清单正文)>`。
先验 MAC 再逐文件核对（防“改清单 + 改文件”同时发生）。

## 10. 一致性与并发约定

- 容器哈希逐块流式计算（不整读文件）；`verify` 零落盘；
- 审计追加前校验“日志尾部 seq/链头 == meta 高水位/head”，不一致直接拒绝（需人工恢复）；
- 备份索引追加在调用方串行化（CLI 监听用互斥锁）；`purge` 用“新索引目录 + 原子换目录”；
- 本地服务每实例一个 JobManager（上限 64 任务），连接带 30 秒收发超时；
- 文件 ACL 收紧用 Win32 `SetFileSecurityW`（SDDL），不调用外部命令。

## 11. 存储与失败语义

| DB/文件 | 性质 | 失败时 |
| --- | --- | --- |
| `vault.keys` | 权威（丢=保险箱打不开） | 拒绝操作并提示备份 |
| `manifest/`、`audit/`、`backups/index/` | 权威（加密日志） | 链校验失败 → 拒绝继续写入 |
| `backups/objects/` | 可重建（从源文件重新备份） | 缺失 → 该版本还原失败 |
| `config.txt` / `dpapi.bin` / `service.token` | 派生/可重建 | 缺失 → 用默认或重新生成 |
