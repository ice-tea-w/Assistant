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

/// 在 root 之下准备一级目录（relDir 可以有多级），缺的逐段创建。
///
/// 与 resolveUnder 的区别：resolveUnder 要求父目录**已存在**（它要解引用 junction
/// 才能复核前缀），所以收文件时不能直接用它 —— 目标子目录往往还没建。
/// 这里改成逐段推进：每推进一级都解引用并复核仍在 root 之下，
/// 这样共享根里被塞了指向外部的 junction 时也不会在外部建目录。
/// 返回规范化后的绝对目录；路径不安全或越界 → 空字符串。
QString ensureDirUnder(const QString &root, const QString &relDir);

} // namespace Sync::PathGuard
