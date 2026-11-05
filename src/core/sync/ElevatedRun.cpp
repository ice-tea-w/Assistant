#include "core/sync/ElevatedRun.h"

#include <QDir>
#include <QProcess>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace Sync {
namespace Os {
namespace {

#ifdef Q_OS_WIN
// Windows 命令行参数引用规则里最常用的部分：含空格或引号才加引号；
// 引号前的反斜杠要成对，引号本身要转义。
QString winQuote(const QString &arg)
{
    static const QRegularExpression needsQuote(QStringLiteral("[\\s\"]"));
    if (!arg.contains(needsQuote))
        return arg;

    QString out = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar c : arg) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (c == QLatin1Char('"')) {
            out += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            out += QLatin1Char('"');
        } else {
            out += QString(backslashes, QLatin1Char('\\'));
            out += c;
        }
        backslashes = 0;
    }
    out += QString(backslashes * 2, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}
#endif

} // namespace

#ifdef Q_OS_WIN

int runElevated(const QString &program, const QStringList &args)
{
    QStringList quoted;
    quoted.reserve(args.size());
    for (const QString &a : args)
        quoted << winQuote(a);

    const std::wstring file = QDir::toNativeSeparators(program).toStdWString();
    const std::wstring params = quoted.join(QLatin1Char(' ')).toStdWString();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        return err == ERROR_CANCELLED ? kElevationCancelled : -static_cast<int>(err);
    }
    if (!sei.hProcess)
        return -1;

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return static_cast<int>(code);
}

#else

int runElevated(const QString &, const QStringList &)
{
    return -1;
}

#endif

int runProgram(const QString &program, const QStringList &args, QString *errorOut)
{
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();

    if (!p.waitForStarted(5000)) {
        if (errorOut)
            *errorOut = p.errorString();
        return -1;
    }
    if (!p.waitForFinished(30000)) {
        p.kill();
        p.waitForFinished(2000);
        if (errorOut)
            *errorOut = QStringLiteral("超时");
        return -1;
    }

    if (errorOut)
        *errorOut = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
}

} // namespace Os
} // namespace Sync
