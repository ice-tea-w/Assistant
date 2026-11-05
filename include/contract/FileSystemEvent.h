#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

/// C2 契约：文件系统变化通知。
///
/// **不是**持久化审计日志，也**不是**保证不丢失的事件流 —— 消费者必须能处理重复、
/// 乱序与溢出（`Overflow` 表示"这一段时间内的变化无法逐条给出，请重新扫描"）。
struct FileSystemEvent
{
    enum Type {
        Added,      ///< 新建（文件或目录）
        Removed,    ///< 删除；`path` 是被删除的旧路径
        Renamed,    ///< 改名；`oldPath` 有效，`path` 是新路径
        Modified,   ///< 内容或属性变化
        Overflow,   ///< 事件队列溢出：`path` 是根目录，消费者应重扫该根
    };

    Type    type = Added;
    QString path;       ///< 新路径（Removed / Overflow 时语义见上）
    QString oldPath;    ///< 仅 Renamed 有效
    qint64  timestamp = 0;   ///< Unix 毫秒

    bool isValid() const { return !path.isEmpty(); }
};

Q_DECLARE_METATYPE(FileSystemEvent)
