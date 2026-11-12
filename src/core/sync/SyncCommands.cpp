#include "core/sync/SyncCommands.h"

#include <QDir>
#include <QFileInfo>

namespace Sync {
namespace {

// 共享名里不允许出现的字符（Windows 文件名非法字符 + 路径分隔符）
const QString &badShareChars()
{
    static const QString chars = QStringLiteral("\\/:*?\"<>|");
    return chars;
}

QString nativePath(const QString &path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QDir::fromNativeSeparators(path)));
}

QString quoted(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + out + QStringLiteral("\"");
}

} // namespace

bool isValidShareName(const QString &name)
{
    if (name.isEmpty() || name.size() > 80)
        return false;
    if (name != name.trimmed())
        return false;
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || badShareChars().contains(c))
            return false;
    }
    return true;
}

QString suggestShareName(const QString &localPath)
{
    // 用 QDir::dirName() 而不是 QFileInfo::fileName()：后者对 "E:/" 会返回 "E:"
    QString name = QDir(nativePath(localPath)).dirName().trimmed();

    QString out;
    out.reserve(name.size());
    for (const QChar c : name)
        out.append((c.unicode() < 0x20 || badShareChars().contains(c)) ? QLatin1Char('_') : c);
    while (out.endsWith(QLatin1Char('.')))
        out.chop(1);

    if (out.isEmpty())
        return QStringLiteral("SyncHub");
    return out.left(80);
}

QString shareScript(const QString &localPath, const QString &shareName, const QString &user)
{
    const QString path = nativePath(localPath);

    QString bat;
    bat += QStringLiteral("@echo off\r\n");
    // 脚本按 UTF-8 写盘：让 cmd 按 UTF-8 读，共享名/账号含中文也不会变乱码
    bat += QStringLiteral("chcp 65001 >nul\r\n");
    bat += QStringLiteral("net share %1=%2 /grant:%3,change\r\n").arg(shareName, quoted(path), user);
    bat += QStringLiteral("if errorlevel 1 exit /b 1\r\n");
    bat += QStringLiteral("icacls %1 /grant %2\r\n")
               .arg(quoted(path), quoted(user + QStringLiteral(":(OI)(CI)M")));
    bat += QStringLiteral("exit /b %errorlevel%\r\n");
    return bat;
}

QStringList connectArgs(const QString &uncPath, const QString &user, const QString &password)
{
    QStringList args{QStringLiteral("use"), nativePath(uncPath), QStringLiteral("/persistent:yes")};
    if (!user.isEmpty()) {
        args << QStringLiteral("/user:") + user;
        if (!password.isEmpty())
            args << password; // 留空 => 用当前登录凭据
    }
    return args;
}

QString explainExitCode(int code, const QString &target)
{
    switch (code) {
    case 0:
        return QStringLiteral("成功");
    case -1000:
        return QStringLiteral("已取消（未授予管理员权限）");
    case 2:
        return QStringLiteral("路径不存在或参数无效：%1").arg(target);
    case 5:
        return QStringLiteral("拒绝访问：当前账号没有创建共享或改该目录权限的资格");
    case 53:
        return QStringLiteral("找不到网络路径：确认对方已开机、主机名正确、防火墙放行 TCP 445，"
                              "且两台机器在同一网段或已 VPN 组网");
    case 67:
        return QStringLiteral("找不到网络名：共享名拼写不对，或共享还没建（目标 %1）").arg(target);
    case 86:
        return QStringLiteral("密码不对：检查账号密码，或改用对方的本机账号（主机名\\用户名）");
    case 1219:
        return QStringLiteral("这台机器已有用其它凭据连到该服务器的会话：先 net use <路径> /delete 再试");
    case 1326:
        return QStringLiteral("登录失败：用户名或密码错误；对方若关掉了密码保护的共享，来宾登录也已被系统默认禁用");
    default:
        return QStringLiteral("命令失败，退出码 %1（目标 %2）").arg(code).arg(target);
    }
}

} // namespace Sync
