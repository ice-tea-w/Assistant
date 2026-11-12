#include "core/RecycleBin.h"

#include "core/Logger.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>
#include <shellapi.h>

#include <vector>

namespace RecycleBin {

bool moveToRecycleBin(const QString &path, QString *error)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (error)
            *error = QStringLiteral("文件不存在：") + path;
        return false;
    }

    const QString native = QDir::toNativeSeparators(path);
    const std::wstring wide = native.toStdWString();
    std::vector<wchar_t> from(wide.begin(), wide.end());
    from.push_back(L'\0');
    from.push_back(L'\0');   // SHFileOperation 需要双 null 结尾

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

    const int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) {
        if (error)
            *error = QStringLiteral("移入回收站失败（SHFileOperation %1）").arg(rc);
        Logger::get().warn("RecycleBin: {} 失败 rc={} aborted={}",
                           path.toStdString(), rc, op.fAnyOperationsAborted ? 1 : 0);
        return false;
    }

    Logger::get().info("RecycleBin: 已移入回收站 {}", path.toStdString());
    return true;
}

} // namespace RecycleBin
