#pragma once

#include "core/FileSnapshot.h"

#include <atomic>
#include <memory>

/// 快照的持有与 **COW（copy-on-write）发布** —— 架构文档 §3.4。
///
/// ```
/// GUI 线程                          工作线程
///   current() ──读──▶ m_current ──原子发布── store(全新快照)
/// ```
///
/// - 写侧（扫描线程）**构建全新快照**，构建完 `publish()` 一次性发布；
///   绝不原地修改已有快照 —— 原地改会迫使查询持锁，与"索引重建期间 UI 不卡"冲突。
/// - 读侧（GUI 线程）`current()` 取一份 `shared_ptr` 副本，**整轮查询无需加锁**：
///   快照不可变，天然线程安全。
/// - 发布原语用 C++20 的 `std::atomic<std::shared_ptr<const T>>`。
///   已实测本机 MSVC（VS2022）支持（见 `docs/devlog/2026-10-15.md` 先决验证），
///   所以不启用 `std::shared_mutex` 降级方案。
class SnapshotManager
{
public:
    SnapshotManager() = default;

    SnapshotManager(const SnapshotManager &) = delete;
    SnapshotManager &operator=(const SnapshotManager &) = delete;

    /// 读侧：拿到当前快照（可能为空 —— 还没扫完/还没载入 DB）。
    /// 返回的 `shared_ptr` 在调用方手里存活期间，指向的对象保证不变。
    std::shared_ptr<const FileSnapshot> current() const;

    /// 写侧：一次性发布新快照，替换旧的。
    void publish(std::shared_ptr<const FileSnapshot> snapshot);

    /// 发布过几代（诊断/验收用）。
    int generation() const;

private:
    std::atomic<std::shared_ptr<const FileSnapshot>> m_current;
    std::atomic<int> m_generation{0};
};
