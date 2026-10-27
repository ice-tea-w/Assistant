#pragma once

#include <QString>

namespace Sync::PathGuard {

/// 路径是否安全（可作为共享根之下的相对路径使用）。
///
/// 拒绝：空路径、绝对路径、盘符形式、".."段、Windows 保留设备名、
///       NTFS 备用数据流（冒号）、段名含控制字符、超长段名。
bool isSafeRelativePath(const QString &relPath);

/// 在 root 之下解析 relPath。
/// - relPath 不安全 → 返回空字符串
/// - 解析后最终路径（含符号链接/junction 解引用）越出 root → 返回空字符串
/// - 正常 → 返回规范化后的绝对路径
QString resolveUnder(const QString &root, const QString &relPath);

} // namespace Sync::PathGuard
