#include "core/AppIndexer.h"
#include "core/Logger.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QElapsedTimer>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#endif

namespace AppIndexer {

#ifdef Q_OS_WIN

/// Resolve a `.lnk` shortcut to its target path using IShellLink + IPersistFile.
/// Returns true and writes the target into @p target on success.
static bool resolveShortcut(const QString &lnkPath, QString *target)
{
    IShellLinkW *shellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW,
                                  reinterpret_cast<void **>(&shellLink));
    if (FAILED(hr) || !shellLink)
        return false;

    bool ok = false;

    IPersistFile *persist = nullptr;
    hr = shellLink->QueryInterface(IID_IPersistFile,
                                   reinterpret_cast<void **>(&persist));
    if (SUCCEEDED(hr) && persist) {
        // IPersistFile::Load expects a wide-char path; QChar is UTF-16 on Windows.
        hr = persist->Load(reinterpret_cast<const wchar_t *>(lnkPath.utf16()),
                           STGM_READ);
        if (SUCCEEDED(hr)) {
            wchar_t buf[4096];
            hr = shellLink->GetPath(buf, 4096, nullptr, 0 /* long path */);
            if (SUCCEEDED(hr) && buf[0] != L'\0') {
                *target = QString::fromWCharArray(buf);
                ok = true;
            }
        }
        persist->Release();
    }

    shellLink->Release();
    return ok;
}

/// Return the filesystem path of a known folder (e.g. FOLDERID_Programs).
static QString knownFolderPath(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        const QString s = QString::fromWCharArray(p);
        CoTaskMemFree(p);
        return s;
    }
    return {};
}

#else

static bool resolveShortcut(const QString &, QString *) { return false; }

#endif

/// Scan one directory for `.lnk` files and resolve each to an Entry.
static QList<Entry> scanDirectory(const QString &dir)
{
    QList<Entry> entries;

    const QDir d(dir);
    if (!d.exists())
        return entries;

    const QFileInfoList files = d.entryInfoList(
        QStringList() << QStringLiteral("*.lnk"), QDir::Files, QDir::Name);

    for (const QFileInfo &fi : files) {
        QString target;
        if (!resolveShortcut(fi.absoluteFilePath(), &target))
            continue;

        Entry e;
        e.name = fi.completeBaseName();  // filename without ".lnk"
        e.path = target;
        entries.append(e);
    }

    return entries;
}

QList<Entry> scan()
{
    QList<Entry> entries;

#ifdef Q_OS_WIN
    // COM must be initialized on this thread before CoCreateInstance.
    // Qt initializes the main thread as STA on Windows, so this usually
    // returns S_FALSE (already initialized) — only uninit if we did the init.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool didInit = (hr == S_OK);
    if (FAILED(hr)) {
        Logger::get().warn("AppIndexer: CoInitializeEx failed (0x{:x})",
                           static_cast<unsigned long>(hr));
    }

    QElapsedTimer timer;
    timer.start();

    const QStringList dirs = {
        knownFolderPath(FOLDERID_Programs),         // per-user Start Menu
        knownFolderPath(FOLDERID_CommonPrograms),   // all-users Start Menu
    };

    for (const QString &dir : dirs) {
        if (dir.isEmpty())
            continue;

        const QList<Entry> found = scanDirectory(dir);
        Logger::get().debug("AppIndexer: {} apps in {}", found.size(),
                            dir.toStdString());
        entries += found;
    }

    // De-duplicate by resolved path (an app may appear in both menus).
    // Windows paths are case-insensitive, so normalize the key to lowercase.
    QList<Entry> unique;
    QSet<QString> seen;
    unique.reserve(entries.size());
    for (const Entry &e : entries) {
        const QString key = e.path.toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        unique.append(e);
    }

    // Stable, friendly order for the default (empty-query) list.
    std::sort(unique.begin(), unique.end(),
              [](const Entry &a, const Entry &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });

    if (didInit)
        CoUninitialize();

    Logger::get().info("AppIndexer: indexed {} apps in {} ms", unique.size(),
                       timer.elapsed());
    return unique;
#else
    return entries;
#endif
}

} // namespace AppIndexer
