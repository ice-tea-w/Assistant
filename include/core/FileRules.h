#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QtGlobal>

#include <cstdint>

#include "core/FileFlags.h"
#include "core/FileKind.h"

/// 检索过滤（L3/L4）：白名单 / 黑名单 / 全都要。
///
/// **它不是采集规则**：切换它只需要从 DB 重建快照，不重扫磁盘（治理文档 §1.1 的分界线）。
struct KindFilter
{
    enum class Mode
    {
        All,        ///< 不做种类白名单，只受排除规则约束（默认）
        Whitelist,  ///< 只保留 `kinds` + `extraExtensions`
        Blacklist,  ///< 排除 `kinds`
    };

    Mode     mode  = Mode::All;
    uint16_t kinds = 0;                 ///< 白名单：接受的位；黑名单：拒绝的位
    QSet<QString> extraExtensions;      ///< 白名单额外放行（小写、不含点）

    /// @param catMask  文件种类位
    /// @param fileName 用于 `extraExtensions` 判定（只看最后一段）
    bool accepts(uint16_t catMask, const QString &fileName) const;

    bool operator==(const KindFilter &other) const
    {
        return mode == other.mode && kinds == other.kinds
            && extraExtensions == other.extraExtensions;
    }
    bool operator!=(const KindFilter &other) const { return !(*this == other); }

    static Mode modeFromString(const QString &text);
    static QString modeToString(Mode mode);
};

/// 采集排除规则（L2）：目录级剪枝 + 文件级规则 + 工程目录识别。
///
/// **这是“表里有什么”的规则**：改了必须重扫磁盘（治理文档 §1.1）。
/// 规则在构造/配置时预折叠小写、预建集合，扫描循环里只做查表。
class ExcludeRuleSet
{
public:
    enum class ProjectPolicy
    {
        Skip,    ///< 整个子树不入库（默认，收益最大）
        Demote,  ///< 入库但打 `FileFlags::Project`，并排除构建产物目录
        Index,   ///< 如实入库
    };

    ExcludeRuleSet();

    // ---- 配置（全部可多次调用；内部会重建查表结构）----
    void setDirNames(const QStringList &names);
    void setDirGlobs(const QStringList &globs);
    void setFileGlobs(const QStringList &globs);
    void setSizeLimits(qint64 minBytes, qint64 maxBytes);
    void setProjectPolicy(ProjectPolicy policy);
    void setProjectSentinels(const QStringList &sentinels);   ///< 精确名与 `*.xxx` 混排
    void setExcludeProjectBuildDirs(bool enabled);

    // ---- 扫描热路径 ----
    bool excludesDirName(const QString &name) const;
    bool excludesFile(const QString &name, qint64 sizeBytes) const;
    bool isProjectSentinel(const QString &name) const;
    bool excludesProjectBuildDir(const QString &name) const;

    // ---- 只读访问（配置回写 / 日志）----
    ProjectPolicy projectPolicy() const { return m_projectPolicy; }
    /// 是否需要做哨兵探测：`Index` 策略不探测（如实入库），Skip/Demote 才探测。
    bool projectGuardEnabled() const { return m_projectGuard && m_projectPolicy != ProjectPolicy::Index; }
    bool excludesProjectBuildDirs() const { return m_excludeBuildDirs; }
    const QStringList &dirNames() const { return m_dirNames; }
    const QStringList &dirGlobs() const { return m_dirGlobs; }
    const QStringList &fileGlobs() const { return m_fileGlobs; }
    const QStringList &projectSentinels() const { return m_sentinels; }
    qint64 minSizeBytes() const { return m_minSize; }
    qint64 maxSizeBytes() const { return m_maxSize; }

    /// 规则集版本（字段哈希）：变了才需要重扫。
    quint64 version() const { return m_version; }

    /// 内置默认目录名单（`node_modules` / `.git` / …）。
    /// `FileScanner::defaultExcludedDirNames()` 返回的就是这一份 —— 名单只有一处。
    static QStringList defaultDirNames();

    /// 简单通配符匹配：`*` / `?`，大小写不敏感，不做路径分隔符特判。
    static bool globMatch(QStringView pattern, QStringView text);

    static ProjectPolicy projectPolicyFromString(const QString &text);
    static QString projectPolicyToString(ProjectPolicy policy);

private:
    void rebuild();
    void touchVersion();

    QStringList m_dirNames;
    QStringList m_dirGlobs;
    QStringList m_fileGlobs;
    QStringList m_sentinels;

    QSet<QString> m_dirNameSet;    ///< 小写
    QSet<QString> m_buildDirSet;   ///< 小写
    QSet<QString> m_sentinelExact; ///< 小写
    QSet<QString> m_fileExtSet;    ///< `*.tmp` 这种纯后缀规则，查扩展名 O(1)
    QStringList   m_fileGlobsGeneral;
    QStringList   m_sentinelGlobs;

    qint64        m_minSize = 0;
    qint64        m_maxSize = 0;   ///< 0 = 不限制
    ProjectPolicy m_projectPolicy = ProjectPolicy::Skip;
    bool          m_projectGuard  = true;
    bool          m_excludeBuildDirs = true;
    quint64       m_version = 0;
};
