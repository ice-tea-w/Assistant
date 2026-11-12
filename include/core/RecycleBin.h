#pragma once

#include <QString>

/// 文件删除只走回收站（治理文档 §2.10）：永不直接 `DeleteFile`。
/// 调用方必须先完成 fullSig 裁决；这里只负责“可撤销地扔进回收站”。
namespace RecycleBin {

bool moveToRecycleBin(const QString &path, QString *error = nullptr);

} // namespace RecycleBin
