#pragma once

#include <QDateTime>
#include <QString>

namespace Sync {

/// 设备名作为投递区目录名时的形态（Windows 非法字符与 '_' 已换成 '-'）。
QString sanitizeDeviceName(const QString &device);

/// 生成投递区里的文件名：<yyyyMMdd-HHmmss>_<原名>。
/// 原名超长时截断并保住扩展名，便于对端还原；reserve 为前缀额外预留的字符数。
QString makeDeliveryFileName(const QString &originalName,
                             const QDateTime &stamp,
                             int reserve = 0);

/// 旧格式（平铺在 _投递/ 根下）：<设备>_<时间戳>_<原名>。
/// 新格式已按设备分子目录，这个只为兼容旧文件保留。
QString makeDeliveryName(const QString &device,
                         const QString &originalName,
                         const QDateTime &stamp);

/// 从投递文件名里剥掉时间戳前缀（新旧两种格式都认），返回原名。
/// 不符合该格式时原样返回。
QString stripDeliveryPrefix(const QString &deliveryName);

/// 旧格式兼容：这个文件名是不是 device 自己发出去的。
bool isOwnDelivery(const QString &deliveryName, const QString &device);

/// 把 srcPath 复制到 destDir/，重名时依次尝试 "原名 (2)"、"原名 (3)"…
/// 返回落地后的绝对路径；复制失败或源不是普通文件时返回空字符串。
QString placeIncoming(const QString &srcPath, const QString &destDir);

} // namespace Sync
