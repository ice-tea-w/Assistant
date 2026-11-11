#include "core/FileScanner.h"

#include "core/FileClassifier.h"
#include "core/FileRules.h"
#include "core/Logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QSet>
#include <QStandardPaths>

#include <utility>
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

/// 工程哨兵前瞻窗口（Phase B）。
///
/// 为什么需要前瞻：句柄栈是**流式**的，进入子目录时还没有它的目录清单，
/// 而"是否整棵跳过"必须在读第一条子项**之前**决定（否则顺序与截断都会变）。
/// 打开子目录时先读最多这么多条到缓冲区里，命中哨兵就整棵丢弃；没命中就把缓冲区
/// 当作正常的待处理条目继续 —— **不额外 stat、不改变条目顺序**。
/// 超过这个窗口才出现哨兵的项目根会被漏判（现实中不存在，且可调大名单弥补）。
constexpr int kProjectProbeEntries = 256;

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

bool isDotOrDotDot(const QString &name)
{
    return name == QLatin1String(".") || name == QLatin1String("..");
}

/// 一条已从 `WIN32_FIND_DATAW` 拷出来的待处理条目。
/// 不直接存 `WIN32_FIND_DATAW`（592 字节/条），前瞻 256 条 × 深度也只要几百 KB。
struct PendingEntry
{
    QString name;
    qint64  size  = 0;
    qint64  mtime = 0;
    DWORD   attrs = 0;

    bool isDirectory() const { return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};

PendingEntry toPending(const WIN32_FIND_DATAW &data)
{
    PendingEntry e;
    e.name  = QString::fromWCharArray(data.cFileName);
    e.size  = (static_cast<qint64>(data.nFileSizeHigh) << 32)
              | static_cast<qint64>(data.nFileSizeLow);
    e.mtime = fileTimeToUnixSec(data.ftLastWriteTime);
    e.attrs = data.dwFileAttributes;
    return e;
}

/// 句柄栈的一层：自己的 `HANDLE` + 前瞻缓冲。
struct Frame
{
    HANDLE        handle = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW data{};
    bool          exhausted = false;   ///< 枚举已经读完（缓冲里可能还有条目）
    QString       dirLong;             ///< `\\?\` 形式（拼子目录用）
    QString       dirNormal;           ///< 普通形式（交给上层）
    bool          isProject = false;   ///< 位于工程子树（demote 策略下打标）
    uint16_t      rootId    = 0;       ///< 扫描根序号（1 起）

    std::vector<PendingEntry> pending;
    std::size_t   pendingPos = 0;
};

/// 打开目录并**前瞻**最多 `probeLimit` 条（用于工程哨兵识别）。
/// probeLimit 至少为 1 —— 第一条也走缓冲，nextEntry 才能统一从缓冲/FindNext 取。
bool openFrame(const QString &dirLong, const QString &dirNormal,
               int probeLimit, bool isProject, uint16_t rootId,
               Frame *frame, DWORD *error)
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

    frame->dirLong     = dirLong;
    frame->dirNormal   = dirNormal;
    frame->isProject   = isProject;
    frame->rootId      = rootId;
    frame->exhausted   = false;
    frame->pending.clear();
    frame->pendingPos  = 0;

    frame->pending.reserve(static_cast<std::size_t>(qBound(1, probeLimit, kProjectProbeEntries)));

    int stored = 0;
    while (stored < probeLimit) {
        const QString name = QString::fromWCharArray(frame->data.cFileName);
        if (!isDotOrDotDot(name)) {
            frame->pending.push_back(toPending(frame->data));
            ++stored;
            // 注意：存满前瞻窗口就**不要**再推进句柄 —— 最后一条已经进了缓冲，
            // 下一条由 nextEntry() 的 FindNextFileW 取。这里多推进一次会丢一条。
            if (stored >= probeLimit)
                break;
        }
        if (!FindNextFileW(frame->handle, &frame->data)) {
            frame->exhausted = true;
            break;
        }
    }

    return true;
}

/// 取下一个条目：先清空前瞻缓冲，再继续 `FindNextFileW`。
bool nextEntry(Frame *frame, PendingEntry *out)
{
    if (frame->pendingPos < frame->pending.size()) {
        *out = std::move(frame->pending[frame->pendingPos]);
        ++frame->pendingPos;
        if (frame->pendingPos == frame->pending.size()) {
            // 缓冲用完就清空（容量留给这一层，Frame 弹栈时一起释放）。不要 shrink_to_fit：
            // 那会让每个目录都多一次分配/释放，扫描吞吐会掉。
            frame->pending.clear();
            frame->pendingPos = 0;
        }
        return true;
    }

    while (!frame->exhausted) {
        if (!FindNextFileW(frame->handle, &frame->data)) {
            frame->exhausted = true;
            return false;
        }
        const QString name = QString::fromWCharArray(frame->data.cFileName);
        if (isDotOrDotDot(name))
            continue;
        *out = toPending(frame->data);
        return true;
    }
    return false;
}

void closeFrame(Frame *frame)
{
    if (frame->handle != INVALID_HANDLE_VALUE) {
        FindClose(frame->handle);
        frame->handle = INVALID_HANDLE_VALUE;
    }
}

bool hasProjectSentinel(const Frame &frame, const ExcludeRuleSet &rules)
{
    for (const PendingEntry &e : frame.pending) {
        if (rules.isProjectSentinel(e.name))
            return true;
    }
    return false;
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
    // 名单只有一份：定义在 ExcludeRuleSet::defaultDirNames()。
    // 函数内静态：线上一处、线程安全、零重复构造。
    static const QStringList kNames = ExcludeRuleSet::defaultDirNames();
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

    const ExcludeRuleSet *rules = options.rules;
    const FileClassifier *classifier = options.classifier;
    const bool projectGuard = rules && rules->projectGuardEnabled();

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

    uint16_t rootOrdinal = 0;
    for (const QString &rootNormal : pendingRoots) {
        if (stats.hitHardLimit || stats.cancelled)
            break;

        ++rootOrdinal;

        Frame rootFrame;
        DWORD err = 0;
        const QString rootLong = toLongPath(toWinSeparators(rootNormal));
        // 根目录不做工程哨兵判定：根是用户显式指定的，哪怕它本身是个工程也要扫。
        if (!openFrame(rootLong, rootNormal, 1, false, rootOrdinal, &rootFrame, &err)) {
            if (err == ERROR_ACCESS_DENIED)
                ++stats.deniedDirs;
            Logger::get().warn("FileScanner: 打不开根 {}（Win32 错误 {}）",
                               rootNormal.toStdString(), static_cast<unsigned>(err));
            continue;
        }

        ++stats.dirsVisited;
        stack.push_back(std::move(rootFrame));

        while (!stack.empty()) {
            if (cancelled()) {
                stats.cancelled = true;
                break;
            }

            Frame &top = stack.back();
            PendingEntry current;
            if (!nextEntry(&top, &current)) {          // 这一层枚举完了
                closeFrame(&top);
                stack.pop_back();
                continue;
            }

            const QString &name = current.name;
            const DWORD attrs = current.attrs;

            // P0-1：不跟随 reparse point（junction / 符号链接 / 挂载点都不下探）
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            if (options.skipHidden && (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                continue;

            if (current.isDirectory()) {
                // P0-2：目录级剪枝 —— 判定一次，整棵子树不进
                const bool excludedByName =
                    rules ? rules->excludesDirName(name)
                          : options.excludedDirNames.contains(name, Qt::CaseInsensitive);
                if (excludedByName) {
                    ++stats.prunedDirs;
                    continue;
                }

                // demote 策略：工程子树里的构建产物目录整棵跳过
                if (rules && top.isProject && rules->excludesProjectBuildDir(name)) {
                    ++stats.prunedDirs;
                    continue;
                }

                Frame child;
                DWORD childErr = 0;
                const QString childNormal = joinNormal(top.dirNormal, name);
                const int probeLimit = projectGuard ? kProjectProbeEntries : 1;
                if (!openFrame(joinLong(top.dirLong, name), childNormal, probeLimit,
                               top.isProject, top.rootId, &child, &childErr)) {
                    // 权限拒绝/被占用：逐目录跳过并计数，不影响其它子树
                    if (childErr == ERROR_ACCESS_DENIED)
                        ++stats.deniedDirs;
                    continue;
                }

                if (projectGuard && hasProjectSentinel(child, *rules)) {
                    if (rules->projectPolicy() == ExcludeRuleSet::ProjectPolicy::Skip) {
                        closeFrame(&child);
                        ++stats.projectDirsSkipped;
                        continue;
                    }
                    if (rules->projectPolicy() == ExcludeRuleSet::ProjectPolicy::Demote) {
                        child.isProject = true;
                        ++stats.projectDirsDemoted;
                    }
                }

                ++stats.dirsVisited;
                stack.push_back(std::move(child));
                continue;
            }

            // ---- 文件级规则 ----
            if (rules && rules->excludesFile(name, current.size)) {
                ++stats.filesSkipped;
                continue;
            }

            if (out) {
                Entry entry;
                entry.dirPath = top.dirNormal;
                entry.name    = name;
                entry.size    = current.size;
                entry.mtime   = current.mtime;
                entry.catMask = classifier ? classifier->classify(name) : 0;
                entry.flags   = top.isProject ? static_cast<uint8_t>(FileFlags::Project)
                                              : static_cast<uint8_t>(FileFlags::None);
                entry.rootId  = top.rootId;
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
