#pragma once

// file_guard · 桌面助手安全模块（C++ 实现）
//
// 唯一实现语言：C++20。文件格式为 C++ 原生格式，不承诺与历史 Python 原型互通，
// 也不提供数据迁移路径（按项目决策）。
//
// 分层：
//   crypto    FGC1 加密容器（PBKDF2-HMAC-SHA256 + 分块 AES-256-GCM）
//   vault     保险箱（DEK + DPAPI + 恢复码 + 名称加密清单 + 口令轮换）
//   scan      敏感文件检测（规则 + 校验位 + docx/xlsx/pptx 正文抽取）
//   audit     审计日志（逐条加密 + HMAC 链 + meta 签名与高水位）
//   backup    备份（内容寻址 + 加密对象/索引 + 版本 + diff + purge + gc）
//   watch     目录监听 + 空闲自动加密
//   service   本地回环 HTTP JSON-RPC（token + Host/Origin + 异步 job）
//   integrity 模块完整性清单（SHA-256 + HMAC）
//
// 用法：
//   #include "file_guard/file_guard.h"
//   auto config = fg::Config::load();
//   fg::crypto::encryptFile("a.txt", "a.txt.fgc", "password");

#include "file_guard/audit.h"
#include "file_guard/backup.h"
#include "file_guard/config.h"
#include "file_guard/crypto.h"
#include "file_guard/errors.h"
#include "file_guard/integrity.h"
#include "file_guard/scan.h"
#include "file_guard/service.h"
#include "file_guard/util.h"
#include "file_guard/vault.h"
#include "file_guard/watch.h"
#include "file_guard/zipx.h"

namespace file_guard {

inline constexpr const char *kProductName = "file_guard";
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr const char *kVersionString = "1.0.0";

} // namespace file_guard

// 便捷别名：业务代码可写 fg::crypto::encryptFile(...)
namespace fg = file_guard;
