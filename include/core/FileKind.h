#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <cstdint>

/// 文件种类位掩码（`docs/architecture/文件治理功能设计.md` §1.3）。
///
/// 一条 `&` 就能做过滤，所以种类**不是**枚举而是 16 位掩码：
///  - 采集期给每个文件打一个 `catMask`（`FileClassifier`）；
///  - 检索期按白名单/黑名单过滤（`KindFilter`）与查询 token（`:doc`）收窄。
///
/// 位值一旦发布就要保持稳定：数据库 `files.cat_mask` 里存的就是这些位。
namespace FileKind {

enum Mask : uint16_t
{
    Document  = 1u << 0,   // doc docx odt rtf txt md tex
    Sheet     = 1u << 1,   // xls xlsx csv ods
    Slide     = 1u << 2,   // ppt pptx odp
    Pdf       = 1u << 3,   // pdf
    Image     = 1u << 4,   // jpg png gif webp bmp svg heic psd ai
    Video     = 1u << 5,   // mp4 mkv avi mov webm flv
    Audio     = 1u << 6,   // mp3 wav flac m4a aac
    Archive   = 1u << 7,   // zip rar 7z tar gz xz
    Installer = 1u << 8,   // exe msi msix appx dmg pkg
    Code      = 1u << 9,   // cpp h py js ts java go rs cs sql sh
    Font      = 1u << 10,  // ttf otf woff woff2
    Data      = 1u << 11,  // json xml yaml toml db sqlite
    Ebook     = 1u << 12,  // epub mobi azw3
    Shortcut  = 1u << 13,  // lnk url
    Disk      = 1u << 14,  // iso img vhd
    Other     = 1u << 15,  // 未命中任何表
};

constexpr uint16_t kAll = 0xFFFFu;

/// 查询 token（`doc` / `:doc` 里的 `doc`）→ 掩码；认不出来返回 0。
///
/// **非法 token 必须返回 0**：`QueryParser` 据此把它降级成自由词，
/// 避免用户输 `:foo` 时结果全空却不知道为什么（治理文档 §1.5）。
uint16_t fromToken(QStringView token);

/// 掩码 → 规范 token（用于配置、日志、UI chip）。多选位时返回空串。
QString token(uint16_t mask);

/// 全部规范 token（文档/测试用）。
QStringList allTokens();

} // namespace FileKind
