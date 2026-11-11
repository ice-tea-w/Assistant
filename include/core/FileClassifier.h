#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <cstdint>

#include "core/FileKind.h"

/// 扩展名 → `FileKind` 掩码（治理文档 §1.3）。
///
/// 设计要点：
///  - **查表 O(1)**：索引期每个文件一次哈希查表，不做字符串解析；
///  - **多段后缀**：`.tar.gz` / `.tar.xz` 按最长已注册后缀判（从左到右试）；
///  - **名称表**：`Makefile` / `.gitignore` / `CMakeLists.txt` 这类没扩展名或
///    扩展名会误导的文件，先按完整文件名查；
///  - **不做内容嗅探**：读文件头会引入 IO 与权限失败路径，收益低于成本。
///
/// 线程安全：构造后只读；`FileIndex` 在 GUI 线程构造一份，扫描 worker 拿到的是**拷贝**。
class FileClassifier
{
public:
    FileClassifier();

    /// 给一个文件名（可含路径，只有最后一段参与判定）打种类位。
    uint16_t classify(const QString &fileName) const;

    /// 用户覆盖：`kindOverrides: { "psd": "image" }`。`ext` 可带前导点。
    void setOverride(const QString &ext, uint16_t mask);
    /// 额外扩展名：没有内置归类时归到 `Other`（白名单模式里会被 `extraExtensions` 接住）。
    void addExtraExtension(const QString &ext);

    /// 已注册扩展名数（诊断用）
    int extensionCount() const { return static_cast<int>(m_ext.size()); }

private:
    void buildDefaults();

    QHash<QString, uint16_t> m_ext;    ///< 小写、不含点；"tar.gz" 这种多段键也在里面
    QHash<QString, uint16_t> m_names;  ///< 完整小写文件名（Makefile / .gitignore …）
};
