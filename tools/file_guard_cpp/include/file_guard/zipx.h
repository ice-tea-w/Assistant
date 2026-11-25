#pragma once

// 最小 ZIP + 原始 DEFLATE 解压（RFC 1951），用于抽取 docx/xlsx/pptx 的 XML 正文。
// 无第三方依赖；带解压上限（防 zip 炸弹）。

#include "file_guard/crypto.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace file_guard::zipx {

struct Entry {
    std::string name;
    std::uint16_t method = 0;      ///< 0 = stored, 8 = deflate
    std::uint32_t compSize = 0;
    std::uint32_t uncompSize = 0;
    std::uint32_t localOffset = 0;
};

/// 列出 ZIP 内的文件条目（目录条目不返回）。
std::vector<Entry> list(const std::filesystem::path &zipPath);

/// 读取一个条目（method 0/8），解压后不得超过 maxOut 字节。
crypto::Bytes readEntry(const std::filesystem::path &zipPath, const Entry &entry,
                        std::size_t maxOut = 8u << 20);

/// 原始 DEFLATE 解压（不含 zlib 头/尾），expectedOut 为声明的解压大小。
crypto::Bytes inflateRaw(const std::uint8_t *data, std::size_t size, std::size_t expectedOut);

} // namespace file_guard::zipx
