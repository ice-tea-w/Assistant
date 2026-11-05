#include "core/SnapshotManager.h"

std::shared_ptr<const FileSnapshot> SnapshotManager::current() const
{
    // acquire：与 publish() 的 release 配对，保证读到的不是"半成品"指针
    return m_current.load(std::memory_order_acquire);
}

void SnapshotManager::publish(std::shared_ptr<const FileSnapshot> snapshot)
{
    m_current.store(std::move(snapshot), std::memory_order_release);
    m_generation.fetch_add(1, std::memory_order_relaxed);
}

int SnapshotManager::generation() const
{
    return m_generation.load(std::memory_order_relaxed);
}
