#pragma once

#include <cstdint>

/// 文件治理标记（`files.flags`，治理文档 §1.6）。
///
/// 与 `FileKind` 一样是位掩码：一条 `&` 判定，不额外占内存。
/// 位值写进数据库，发布后保持稳定。
namespace FileFlags {
enum : uint8_t
{
    None      = 0,
    Project   = 1u << 0,   ///< 位于被识别为“工程”的目录（demote 策略下入库但降权）
    Inbox     = 1u << 1,   ///< 下载 box 中转站里的文件（Phase C）
    Hidden    = 1u << 2,   ///< 保留位（skipHidden 目前直接不收）
    Duplicate = 1u << 3,   ///< 查重命中（Phase E）
};
} // namespace FileFlags
