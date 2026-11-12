#pragma once

#include <QString>
#include <QtGlobal>

/// 文件身份指纹（治理文档 §2.6 第一步 / §2.10）。
///
/// 三种身份用途不同，**不可互相替代**：
///  - 物理身份 `(volumeSerial, fileId)`：同卷改名/移动后不变（NTFS FRN），只在同卷有效；
///  - 内容身份 `quickSig`：`Sha256(size ‖ 首 64KB ‖ 尾 64KB)`，跨卷有效、概率性；
///  - 元数据身份 `(size, mtime)`：最便宜，只用于缩小候选，**绝不单独确认**。
namespace FileIdentity {

struct Identity
{
    quint32 volumeSerial = 0;
    QString fileId;      ///< 128-bit FileId 的十六进制（32 字符）；取不到为空
    qint64  size  = 0;
    qint64  mtime = 0;   ///< Unix 秒
};

/// 读取物理身份 + 元数据。文件不存在 / 无权限时返回 false（不抛出）。
bool read(const QString &path, Identity *out);

/// 快速内容签名。读失败返回空串。大文件只读首尾各 64 KB。
QString quickSignature(const QString &path);

/// 全量内容签名（SHA-256 整个文件）。只在“查重裁决”时用，别在采集路径上调用。
QString fullSignature(const QString &path);

/// 当前时刻能否以**独占共享模式**打开（§2.3 条件 C）。
/// 返回 true 只说明"此刻没有共享冲突"，不是"此后不会再被写"。
bool canOpenExclusive(const QString &path, bool *sharingViolation = nullptr);

} // namespace FileIdentity
