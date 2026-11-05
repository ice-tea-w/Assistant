#pragma once

#include <QVector>

#include <functional>
#include <vector>

#include "contract/FileSystemEvent.h"

/// 极轻量的消费者注册表 —— **不是** 全局 EventBus。
///
/// 存在理由只有一个：`FileIndex`、`TransferManager`、账本等消费者需要各自订阅，
/// 而 `FileWatcher` 不应该认识它们中的任何一个。一个进程内实例即可，但必须由
/// 组合根（main.cpp）显式构造并传递，不做单例。
///
/// 线程：`publish()` 在哪个线程调用，回调就在哪个线程执行；需要跨线程的消费者
/// 自己投递（例如 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`）。
class FileEventDispatcher
{
public:
    using Handler = std::function<void(const FileSystemEvent &)>;

    /// 返回订阅号，用于 unsubscribe。
    int subscribe(Handler handler);

    void unsubscribe(int id);

    /// 依次调用所有订阅者；单个订阅者抛异常不影响其它订阅者。
    void publish(const QVector<FileSystemEvent> &batch);

    void publish(const FileSystemEvent &event);

    void clear();

    int subscriberCount() const;

private:
    struct Entry {
        int id;
        Handler handler;
    };

    std::vector<Entry> m_handlers;
    int m_nextId = 1;
};
