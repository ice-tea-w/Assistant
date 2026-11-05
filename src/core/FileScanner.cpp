#include "core/FileScanner.h"
#include "core/Logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QSet>
#include <QStandardPaths>

#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace FileScanner {

namespace {

/// 1601-01-01 到 1970-01-01 之间的秒数（FILETIME 的纪元差）
constexpr qint64 kFileTimeEpochOffsetSec = 11644473600LL;

QString toWinSeparators(const QString &path)
{
    QString p = path;
    p.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return p;
}

/// 转成 `\\?\` 长路径形式（Win32 只对带前缀的路径解除 MAX_PATH 限制）
QString toLongPath(const QString &winPath)
{
    if (winPath.startsWith(QStringLiteral("\\\\?\\")))
        return winPath;
    if (winPath.startsWith(QStringLiteral("\\\\")))          // UNC: \\srv\share → \\?\UNC\srv\share
        return QStringLiteral("\\\\?\\UNC\\") + winPath.mid(2);
    return QStringLiteral("\\\\?\\") + winPath;
}

/// 去掉 `\\?\`，并统一成 `/` 分隔（存进快照/DB 的形式）
QString fromLongPath(const QString &longPath)
{
    QString p = longPath;
    if (p.startsWith(QStringLiteral("\\\\?\\UNC\\")))
        p = QStringLiteral("\\\\") + p.mid(8);
    else if (p.startsWith(QStringLiteral("\\\\?\\")))
        p = p.mid(4);
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p;
}

QString joinNormal(const QString &dir, const QString &name)
{
    if (dir.isEmpty())
        return name;
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

QString joinLong(const QString &dir, const QString &name)
{
    // 只有盘符根（\\?\C:\）会以反斜杠结尾，别拼出双反斜杠
    return dir.endsWith(QLatin1Char('\\')) ? dir + name : dir + QLatin1Char('\\') + name;
}

qint64 fileTimeToUnixSec(const FILETIME &ft)
{
    ULARGE_INTEGER ticks;
    ticks.LowPart  = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    return static_cast<qint64>(ticks.QuadPart / 10000000ULL) - kFileTimeEpochOffsetSec;
}

/// 句柄栈的一层。`data` 里始终装着"当前待处理的那一条"。
struct Frame
{
    HANDLE           handle = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW data{};
    bool             firstPending = false;   ///< data 里是 FindFirstFileExW 的返回值
    QString          dirLong;                ///< `\\?\` 形式（拼子目录用）
    QString          dirNormal;              ///< 普通形式（交给上层）
};

bool openFrame(const QString &dirLong, const QString &dirNormal, Frame *frame, DWORD *error)
{
    QString pattern = dirLong;
    if (!pattern.endsWith(QLatin1Char('\\')))
        pattern += QLatin1Char('\\');
    pattern += QLatin1Char('*');

    frame->data = WIN32_FIND_DATAW{};
    frame->handle = FindFirstFileExW(reinterpret_cast<LPCWSTR>(pattern.utf16()),
                                     FindExInfoBasic,
                                     &frame->data,
                                     FindExSearchNameMatch,
                                     nullptr,
                                     FIND_FIRST_EX_LARGE_FETCH);
    if (frame->handle == INVALID_HANDLE_VALUE) {
        if (error)
            *error = GetLastError();
        return false;
    }

    frame->firstPending = true;
    frame->dirLong      = dirLong;
    frame->dirNormal    = dirNormal;
    return true;
}

bool nextEntry(Frame *frame)
{
    if (frame->firstPending) {
        frame->firstPending = false;
        return true;
    }
    return FindNextFileW(frame->handle, &frame->data) != 0;
}

void closeFrame(Frame *frame)
{
    if (frame->handle != INVALID_HANDLE_VALUE) {
        FindClose(frame->handle);
        frame->handle = INVALID_HANDLE_VALUE;
    }
}

bool isDotOrDotDot(const QString &name)
{
    return name == QLatin1String(".") || name == QLatin1String("..");
}

} // namespace

QString Entry::path() const
{
    return joinNormal(dirPath, name);
}

QStringList defaultRoots()
{
    return {
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
    };
}

const QStringList &defaultExcludedDirNames()
{
    // 函数内静态：线上一处、线程安全、零重复构造
    static const QStringList kNames = {
        QStringLiteral("node_modules"),
        QStringLiteral("__pycache__"),
        QStringLiteral(".cache"),
        QStringLiteral(".git"),
        QStringLiteral(".svn"),
    };
    return kNames;
}

Stats scan(const QStringList &roots,
           const Options &options,
           QVector<Entry> *out,
           const std::function<bool()> &shouldCancel)
{
    Stats stats;
    QElapsedTimer timer;
    timer.start();

    if (out) {
        out->clear();
        out->reserve(4096);
    }

    // ---- 根目录预处理：去重 + 只留真实存在的目录 ----
    QStringList pendingRoots;          // 普通形式
    {
        QSet<QString> seen;
        for (const QString &raw : roots) {
            if (raw.isEmpty())
                continue;

            QString normal = QDir::fromNativeSeparators(raw);
            while (normal.size() > 3 && normal.endsWith(QLatin1Char('/')))
                normal.chop(1);
            if (seen.contains(normal))
                continue;
            seen.insert(normal);

            const QString longPath = toLongPath(toWinSeparators(normal));
            const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(longPath.utf16()));
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                Logger::get().debug("FileScanner: 跳过根 {}（不存在或不是目录）", normal.toStdString());
                continue;
            }
            pendingRoots.append(normal);
        }
    }

    std::vector<Frame> stack;
    stack.reserve(32);

    auto cancelled = [&shouldCancel]() { return shouldCancel && shouldCancel(); };

    for (const QString &rootNormal : pendingRoots) {
        if (stats.hitHardLimit || stats.cancelled)
            break;

        Frame rootFrame;
        DWORD err = 0;
        const QString rootLong = toLongPath(toWinSeparators(rootNormal));
        if (!openFrame(rootLong, rootNormal, &rootFrame, &err)) {
            if (err == ERROR_ACCESS_DENIED)
                ++stats.deniedDirs;
            Logger::get().warn("FileScanner: 打不开根 {}（Win32 错误 {}）",
                               rootNormal.toStdString(), static_cast<unsigned>(err));
            continue;
        }

        ++stats.dirsVisited;
        stack.push_back(rootFrame);

        while (!stack.empty()) {
            if (cancelled()) {
                stats.cancelled = true;
                break;
            }

            Frame &top = stack.back();
            if (!nextEntry(&top)) {          // 这一层枚举完了
                closeFrame(&top);
                stack.pop_back();
                continue;
            }

            const WIN32_FIND_DATAW &data = top.data;
            const QString name = QString::fromWCharArray(data.cFileName);
            if (isDotOrDotDot(name))
                continue;

            const DWORD attrs = data.dwFileAttributes;

            // P0-1：不跟随 reparse point（junction / 符号链接 / 挂载点都不下探）
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            if (options.skipHidden && (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                continue;

            if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                // P0-2：目录级剪枝 —— 判定一次，整棵子树不进
                if (options.excludedDirNames.contains(name, Qt::CaseInsensitive)) {
                    ++stats.prunedDirs;
                    continue;
                }

                Frame child;
                DWORD childErr = 0;
                const QString childNormal = joinNormal(top.dirNormal, name);
                if (!openFrame(joinLong(top.dirLong, name), childNormal, &child, &childErr)) {
                    // 权限拒绝/被占用：逐目录跳过并计数，不影响其它子树
                    if (childErr == ERROR_ACCESS_DENIED)
                        ++stats.deniedDirs;
                    continue;
                }

                ++stats.dirsVisited;
                stack.push_back(std::move(child));
                continue;
            }

            if (out) {
                Entry entry;
                entry.dirPath = top.dirNormal;
                entry.name    = name;
                entry.size    = (static_cast<qint64>(data.nFileSizeHigh) << 32)
                                | static_cast<qint64>(data.nFileSizeLow);
                entry.mtime   = fileTimeToUnixSec(data.ftLastWriteTime);
                out->append(std::move(entry));
            }
            ++stats.files;

            if (stats.files >= options.maxEntries) {
                stats.hitHardLimit = true;
                break;
            }
        }

        // 取消或撞上限时，这一轮还有句柄没关
        for (Frame &frame : stack)
            closeFrame(&frame);
        stack.clear();
    }

    stats.elapsedMs = timer.elapsed();
    return stats;
}

} // namespace FileScanner
