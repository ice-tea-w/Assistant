#include "core/FileEventDispatcher.h"

#include "core/Logger.h"

#include <exception>

int FileEventDispatcher::subscribe(Handler handler)
{
    if (!handler)
        return 0;
    const int id = m_nextId++;
    m_handlers.push_back({id, std::move(handler)});
    return id;
}

void FileEventDispatcher::unsubscribe(int id)
{
    for (auto it = m_handlers.begin(); it != m_handlers.end(); ++it) {
        if (it->id == id) {
            m_handlers.erase(it);
            return;
        }
    }
}

void FileEventDispatcher::publish(const QVector<FileSystemEvent> &batch)
{
    for (const FileSystemEvent &ev : batch)
        publish(ev);
}

void FileEventDispatcher::publish(const FileSystemEvent &event)
{
    // 拷贝一份：回调里可能会 unsubscribe，直接迭代原容器会失效
    const std::vector<Entry> handlers = m_handlers;
    for (const Entry &e : handlers) {
        try {
            e.handler(event);
        } catch (const std::exception &ex) {
            Logger::get().error("FileEventDispatcher: subscriber {} threw: {}",
                                e.id, ex.what());
        } catch (...) {
            Logger::get().error("FileEventDispatcher: subscriber {} threw unknown",
                                e.id);
        }
    }
}

void FileEventDispatcher::clear()
{
    m_handlers.clear();
}

int FileEventDispatcher::subscriberCount() const
{
    return static_cast<int>(m_handlers.size());
}
