#pragma once

#include <QObject>
#include <QStringList>
#include <QVector>

#include <memory>

#include "contract/FileSystemEvent.h"

/// 增量事件源：每个根目录一个句柄、`bWatchSubtree = TRUE`。
///
/// 三条硬规则（`docs/architecture/v0.4-实施清单.md` §A5）：
///  1. 只负责"发现了变化"，**不认识任何具体模块**；
///  2. 事件按路径合并去重（默认 100 ms 窗口），抖动源（网盘同步、编译输出）不应
///     把下游淹没；
///  3. `ReadDirectoryChangesW` 失败且错误是 `ERROR_NOTIFY_ENUM_DIR` 时**发 `Overflow`**，
///     由消费者重扫该根 —— 不允许静默丢事件。
///
/// 线程模型：每个根一个工作线程阻塞在 `ReadDirectoryChangesW` 上；合并与派发在
/// 本对象所属线程（有事件循环时是 GUI 线程）通过 `QTimer` 完成，因此
/// `batchReady` 总是在该线程发出。
class FileWatcher : public QObject
{
    Q_OBJECT

public:
    explicit FileWatcher(QObject *parent = nullptr);
    ~FileWatcher() override;

    /// 开始监听若干根目录。不存在的路径会被跳过；至少一个成功才返回 true。
    /// 重复调用会先 stop()。
    bool start(const QStringList &roots);

    /// 停止并 join 所有工作线程。可重复调用。
    void stop();

    bool isRunning() const;
    QStringList roots() const;

    /// 合并窗口（毫秒）。0 表示不合并、逐条发出。
    void setCoalesceMs(int ms);
    int  coalesceMs() const;

signals:
    /// 一批事件（已按路径去重）。`Overflow` 不会被合并掉。
    void batchReady(const QVector<FileSystemEvent> &events);

public:
    // 实现体（定义在 .cpp 里）；放在 public 只为让同一编译单元的辅助函数能引用它
    struct Impl;

private:
    std::unique_ptr<Impl> d;
};
