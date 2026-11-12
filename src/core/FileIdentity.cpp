#include "core/FileIdentity.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace FileIdentity {

namespace {

constexpr qint64 kSignatureChunk = 64 * 1024;

QString toLongPath(const QString &path)
{
    QString win = path;
    win.replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (win.startsWith(QStringLiteral("\\\\?\\")))
        return win;
    if (win.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + win.mid(2);
    return QStringLiteral("\\\\?\\") + win;
}

QString fileIdToHex(const FILE_ID_INFO &info)
{
    // FileId 是 128-bit；按字节输出 32 个 hex 字符，大小写无关（存库统一小写）
    static const char kHex[] = "0123456789abcdef";
    QString out;
    out.reserve(32);
    for (unsigned char byte : info.FileId.Identifier) {
        out.append(QLatin1Char(kHex[byte >> 4]));
        out.append(QLatin1Char(kHex[byte & 0x0F]));
    }
    return out;
}

} // namespace

bool read(const QString &path, Identity *out)
{
    if (!out)
        return false;

    const QString longPath = toLongPath(path);
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(longPath.utf16()),
                           FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    BY_HANDLE_FILE_INFORMATION basic{};
    FILE_ID_INFO idInfo{};
    const bool basicOk = GetFileInformationByHandle(h, &basic) != 0;
    const bool idOk = GetFileInformationByHandleEx(h, FileIdInfo, &idInfo, sizeof(idInfo)) != 0;
    CloseHandle(h);

    if (!basicOk)
        return false;

    out->volumeSerial = idOk ? idInfo.VolumeSerialNumber : basic.dwVolumeSerialNumber;
    out->fileId = idOk ? fileIdToHex(idInfo) : QString();
    out->size = (static_cast<qint64>(basic.nFileSizeHigh) << 32)
                | static_cast<qint64>(basic.nFileSizeLow);

    ULARGE_INTEGER ticks;
    ticks.LowPart  = basic.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = basic.ftLastWriteTime.dwHighDateTime;
    constexpr qint64 kEpochOffsetSec = 11644473600LL;
    out->mtime = static_cast<qint64>(ticks.QuadPart / 10000000ULL) - kEpochOffsetSec;
    return true;
}

QString quickSignature(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    const qint64 size = file.size();
    QCryptographicHash hash(QCryptographicHash::Sha256);

    // size_le64：把长度按小端写进签名，避免"同首尾不同长度"撞车
    quint64 le = static_cast<quint64>(size);
    unsigned char lengthBytes[8];
    for (int i = 0; i < 8; ++i)
        lengthBytes[i] = static_cast<unsigned char>((le >> (8 * i)) & 0xFF);
    hash.addData(QByteArray(reinterpret_cast<const char *>(lengthBytes), 8));

    const QByteArray head = file.read(qMin(size, kSignatureChunk));
    hash.addData(head);

    if (size > kSignatureChunk) {
        const qint64 tailStart = qMax<qint64>(kSignatureChunk, size - kSignatureChunk);
        if (file.seek(tailStart))
            hash.addData(file.read(size - tailStart));
    }

    file.close();
    return QString::fromLatin1(hash.result().toHex());
}

bool canOpenExclusive(const QString &path, bool *sharingViolation)
{
    if (sharingViolation)
        *sharingViolation = false;

    const QString longPath = toLongPath(path);
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(longPath.utf16()),
                           GENERIC_READ,
                           0,                    // dwShareMode = 0 → 独占
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return true;
    }

    if (sharingViolation) {
        const DWORD err = GetLastError();
        *sharingViolation = (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION);
    }
    return false;
}

} // namespace FileIdentity
