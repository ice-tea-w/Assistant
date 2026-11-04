#pragma once

#include <QString>
#include <QtGlobal>

#include <memory>
#include <utility>
#include <vector>

/// 文件索引的**不可变快照** —— 查询层唯一允许接触的数据表示。
///
/// 硬约束（见 `docs/architecture/文件检索重构方案.md` §3.1）：物理表示藏在只读
/// 访问器之后，查询代码只认 `size()` + `xFooAt(i)`，**永远不直接摸容器**。这样将来
/// 换 SoA / 目录字典 / UTF-8 表示时，改动只落在这个文件里，不波及查询层与测试。
///
/// v1 物理表示：`std::vector<FileEntry>`（3 个 QString + 2 个标量）。20 万条约
/// 50–70 MB，低于预算，且"省内存"的手段都不是当前已知的瓶颈（全部 gated）。
class FileSnapshot
{
public:
    /// 一条文件记录。**只有 Builder 和 FileSnapshot 自己碰它**；
    /// 查询层请走下面的只读访问器。
    struct FileEntry
    {
        QString name;       // 原始大小写（展示用）
        QString nameLower;  // 预折叠小写（匹配用）—— 消除每按键 N 次 toLower() 分配
        QString path;       // 绝对路径（**定位键，不是文件身份**，见 §3.6）
        qint64  size  = 0;  // 字节
        qint64  mtime = 0;  // Unix 秒
    };

    /// 构建期写入器：扫描线程用它攒数据，扫完 `finish()` 得到可发布的快照。
    /// 快照发布后不可变，所以**写入只能发生在这里**。
    class Builder
    {
    public:
        void reserve(int n) { m_entries.reserve(static_cast<std::size_t>(n)); }

        /// 追加一条。`nameLower` 在这里算一次（不在每条查询里算 N 次）。
        void add(const QString &name, const QString &path, qint64 size, qint64 mtime)
        {
            FileEntry e;
            e.name      = name;
            e.nameLower = name.toLower();
            e.path      = path;
            e.size      = size;
            e.mtime     = mtime;
            m_entries.push_back(std::move(e));
        }

        int count() const { return static_cast<int>(m_entries.size()); }

        /// 交出结果。调用后 Builder 不可再用。
        std::shared_ptr<FileSnapshot> finish()
        {
            auto snap = std::make_shared<FileSnapshot>();
            snap->m_entries = std::move(m_entries);
            m_entries.clear();
            return snap;
        }

    private:
        std::vector<FileEntry> m_entries;
    };

    // ---- 只读接口（查询层唯一入口） ----

    int  size()  const { return static_cast<int>(m_entries.size()); }
    bool empty() const { return m_entries.empty(); }

    QString nameAt(int i) const { return at(i).name; }   // 展示：调用者拥有副本
    QString pathAt(int i) const { return at(i).path; }   // 定位：调用者拥有副本
    qint64  sizeAt(int i) const { return at(i).size; }
    qint64  mtimeAt(int i) const { return at(i).mtime; }

    /// 匹配热路径：**零拷贝**，故意返回 const 引用而不是 QString。
    ///
    /// 每条查询要对全表每个条目调一次；按值返回等于每次按键复制 20 万个字符串。
    /// 生命周期绑定快照本身 —— 查询期间调用方持有 `shared_ptr`，所以安全。
    const QString &lowerAt(int i) const { return at(i).nameLower; }

private:
    const FileEntry &at(int i) const { return m_entries[static_cast<std::size_t>(i)]; }

    std::vector<FileEntry> m_entries;
};
