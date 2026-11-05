#pragma once

#include <QString>
#include <QStringList>

namespace Sync {
namespace Os {

/// UAC 被取消（用户点了"否"）
constexpr int kElevationCancelled = -1000;

/// 以管理员权限执行并等它结束，返回退出码。
/// 参数逐个加引号后拼成 lpParameters（不经过 shell，不存在注入面）。
int runElevated(const QString &program, const QStringList &args);

/// 当前权限同步执行（net use 用），返回退出码；起不来/超时/被杀返回 -1。
/// errorOut 收 stderr（net 的错误信息在 stderr）。
int runProgram(const QString &program, const QStringList &args, QString *errorOut = nullptr);

} // namespace Os
} // namespace Sync
