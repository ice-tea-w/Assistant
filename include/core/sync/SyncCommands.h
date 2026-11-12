#pragma once

#include <QString>
#include <QStringList>

namespace Sync {

/// 共享名合法性：1..80 字符，不含 \ / : * ? " < > | 与控制字符，首尾不能是空格
bool isValidShareName(const QString &name);

/// 从本地目录名推断共享名：非法字符换成 _，空则回退 "SyncHub"
QString suggestShareName(const QString &localPath);

/// 主机侧 .bat 内容（CRLF 换行）。用管理员权限执行：
/// net share <name>=<path> /grant:<user>,change
/// 再补 NTFS 的「修改」权限（共享权限 ∩ NTFS 才是最终权限）
QString shareScript(const QString &localPath, const QString &shareName, const QString &user);

/// 连接侧 `net use` 的参数列表（不含 net 本身）。password 为空则不带密码，
/// 用当前登录凭据；user 为空则不加 /user。
QStringList connectArgs(const QString &uncPath, const QString &user, const QString &password);

/// net / icacls 退出码 → 给用户的中文解释。target 用于把路径写进消息。
QString explainExitCode(int code, const QString &target);

} // namespace Sync
