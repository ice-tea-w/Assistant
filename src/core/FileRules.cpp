#include "core/FileRules.h"

#include <QHash>

namespace {

/// 去空白、去重（大小写不敏感），但**保留用户输入的原始大小写**：
/// 配置回写时不该把 `CMakeLists.txt` 变成 `cmakelists.txt`。
/// 匹配用的集合在 `rebuild()` 里单独折叠小写。
QStringList normalized(const QStringList &input)
{
    QStringList out;
    QSet<QString> seen;
    out.reserve(input.size());
    for (const QString &raw : input) {
        const QString s = raw.trimmed();
        if (s.isEmpty())
            continue;
        const QString key = s.toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out.append(s);
    }
    return out;
}

QString extensionOf(const QString &name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot + 1 >= name.size())
        return QString();
    return name.mid(dot + 1).toLower();
}

} // namespace

// ---------------------------------------------------------------------------
// KindFilter
// ---------------------------------------------------------------------------

bool KindFilter::accepts(uint16_t catMask, const QString &fileName) const
{
    const int slash = fileName.lastIndexOf(QLatin1Char('/'));
    const QString name = (slash >= 0 ? fileName.mid(slash + 1) : fileName).toLower();
    return acceptsLower(catMask, QStringView(name));
}

bool KindFilter::acceptsLower(uint16_t catMask, QStringView lowerName) const
{
    if (mode == Mode::All)
        return true;

    if (mode == Mode::Blacklist)
        return (catMask & kinds) == 0;

    // Whitelist
    if ((catMask & kinds) != 0)
        return true;

    if (extraExtensions.isEmpty())
        return false;

    const qsizetype dot = lowerName.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot + 1 >= lowerName.size())
        return false;

    const QStringView ext = lowerName.mid(dot + 1);
    for (const QString &wanted : extraExtensions) {
        if (ext == QStringView(wanted))
            return true;
    }
    return false;
}

KindFilter::Mode KindFilter::modeFromString(const QString &text)
{
    const QString s = text.trimmed().toLower();
    if (s == QLatin1String("whitelist") || s == QLatin1String("only"))
        return Mode::Whitelist;
    if (s == QLatin1String("blacklist") || s == QLatin1String("exclude"))
        return Mode::Blacklist;
    return Mode::All;
}

QString KindFilter::modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Whitelist: return QStringLiteral("whitelist");
    case Mode::Blacklist: return QStringLiteral("blacklist");
    default:              return QStringLiteral("all");
    }
}

// ---------------------------------------------------------------------------
// ExcludeRuleSet
// ---------------------------------------------------------------------------

ExcludeRuleSet::ExcludeRuleSet()
{
    m_dirNames = defaultDirNames();
    m_fileGlobs = {
        QStringLiteral("*.tmp"), QStringLiteral("*.log"), QStringLiteral("*.bak"),
        QStringLiteral("~$*"),   QStringLiteral("*.crdownload"), QStringLiteral("*.part"),
    };
    // 工程哨兵（治理文档 §1.2）：精确名 + `*.sln` 这类通配
    m_sentinels = normalized({
        QStringLiteral(".git"), QStringLiteral(".hg"), QStringLiteral(".svn"),
        QStringLiteral("CMakeLists.txt"), QStringLiteral("Makefile"), QStringLiteral("meson.build"),
        QStringLiteral("package.json"), QStringLiteral("pnpm-lock.yaml"),
        QStringLiteral("yarn.lock"), QStringLiteral("tsconfig.json"),
        QStringLiteral("Cargo.toml"), QStringLiteral("go.mod"),
        QStringLiteral("pyproject.toml"), QStringLiteral("requirements.txt"),
        QStringLiteral(".venv"), QStringLiteral("venv"),
        QStringLiteral("build.gradle"), QStringLiteral("pom.xml"),
        QStringLiteral("Gemfile"), QStringLiteral("composer.json"),
        QStringLiteral("*.sln"), QStringLiteral("*.vcxproj"), QStringLiteral("*.csproj"),
    });
    m_projectPolicy = ProjectPolicy::Skip;
    m_excludeBuildDirs = true;
    rebuild();
}

QStringList ExcludeRuleSet::defaultDirNames()
{
    // 名单只有这一份：FileScanner::defaultExcludedDirNames() 也返回它。
    // 刻意只放“跨用户都确定是噪音”的目录名；`build` / `bin` / `out` 这类
    // 只在“工程目录内部”（demote 策略）才剪，否则会把用户的普通文件夹误剪。
    return {
        QStringLiteral("node_modules"),
        QStringLiteral("__pycache__"),
        QStringLiteral(".cache"),
        QStringLiteral(".git"),
        QStringLiteral(".svn"),
    };
}

void ExcludeRuleSet::setDirNames(const QStringList &names)
{
    m_dirNames = normalized(names);
    rebuild();
    touchVersion();
}

void ExcludeRuleSet::setDirGlobs(const QStringList &globs)
{
    m_dirGlobs = normalized(globs);
    rebuild();
    touchVersion();
}

void ExcludeRuleSet::setFileGlobs(const QStringList &globs)
{
    m_fileGlobs = normalized(globs);
    rebuild();
    touchVersion();
}

void ExcludeRuleSet::setSizeLimits(qint64 minBytes, qint64 maxBytes)
{
    m_minSize = qMax<qint64>(0, minBytes);
    m_maxSize = qMax<qint64>(0, maxBytes);
    touchVersion();
}

void ExcludeRuleSet::setProjectPolicy(ProjectPolicy policy)
{
    if (m_projectPolicy == policy)
        return;
    m_projectPolicy = policy;
    touchVersion();
}

void ExcludeRuleSet::setProjectSentinels(const QStringList &sentinels)
{
    m_sentinels = normalized(sentinels);
    m_projectGuard = !m_sentinels.isEmpty();
    rebuild();
    touchVersion();
}

void ExcludeRuleSet::setExcludeProjectBuildDirs(bool enabled)
{
    if (m_excludeBuildDirs == enabled)
        return;
    m_excludeBuildDirs = enabled;
    touchVersion();
}

void ExcludeRuleSet::rebuild()
{
    m_dirNameSet.clear();
    for (const QString &name : m_dirNames)
        m_dirNameSet.insert(name.toLower());

    m_buildDirSet = {
        QStringLiteral("build"), QStringLiteral("out"), QStringLiteral("target"),
        QStringLiteral("bin"), QStringLiteral("obj"), QStringLiteral("dist"),
        QStringLiteral(".vs"), QStringLiteral("deriveddata"),
        QStringLiteral("cmake-build-debug"), QStringLiteral("cmake-build-release"),
    };

    m_sentinelExact.clear();
    m_sentinelGlobs.clear();
    for (const QString &s : m_sentinels) {
        if (s.contains(QLatin1Char('*')) || s.contains(QLatin1Char('?')))
            m_sentinelGlobs.append(s.toLower());
        else
            m_sentinelExact.insert(s.toLower());
    }

    m_fileExtSet.clear();
    m_fileGlobsGeneral.clear();
    for (const QString &pattern : m_fileGlobs) {
        // "*.tmp" 这种纯后缀规则退化成扩展名查表（O(1)）
        if (pattern.size() > 2 && pattern.startsWith(QLatin1String("*."))
            && !pattern.mid(2).contains(QLatin1Char('*'))
            && !pattern.mid(2).contains(QLatin1Char('?'))) {
            m_fileExtSet.insert(pattern.mid(2).toLower());
        } else {
            m_fileGlobsGeneral.append(pattern);
        }
    }
}

void ExcludeRuleSet::touchVersion()
{
    quint64 h = 1469598103934665603ULL;   // FNV offset
    auto mix = [&h](const QString &s) {
        for (const QChar c : s) {
            h ^= static_cast<quint64>(c.unicode());
            h *= 1099511628211ULL;
        }
        h ^= 0xFF;
        h *= 1099511628211ULL;
    };

    for (const QString &s : m_dirNames) mix(s);
    for (const QString &s : m_dirGlobs) mix(s);
    for (const QString &s : m_fileGlobs) mix(s);
    for (const QString &s : m_sentinels) mix(s);
    mix(QString::number(m_minSize));
    mix(QString::number(m_maxSize));
    mix(QString::number(static_cast<int>(m_projectPolicy)));
    mix(QString::number(m_excludeBuildDirs ? 1 : 0));
    m_version = h;
}

bool ExcludeRuleSet::excludesDirName(const QString &name) const
{
    const QString lower = name.toLower();
    if (m_dirNameSet.contains(lower))
        return true;
    for (const QString &glob : m_dirGlobs) {
        if (globMatch(glob, lower))
            return true;
    }
    return false;
}

bool ExcludeRuleSet::excludesFile(const QString &name, qint64 sizeBytes) const
{
    if (m_minSize > 0 && sizeBytes < m_minSize)
        return true;
    if (m_maxSize > 0 && sizeBytes > m_maxSize)
        return true;

    const QString lower = name.toLower();
    if (!m_fileExtSet.isEmpty()) {
        const QString ext = extensionOf(lower);
        if (!ext.isEmpty() && m_fileExtSet.contains(ext))
            return true;
    }
    for (const QString &glob : m_fileGlobsGeneral) {
        if (globMatch(glob, lower))
            return true;
    }
    return false;
}

bool ExcludeRuleSet::isProjectSentinel(const QString &name) const
{
    if (!m_projectGuard)
        return false;

    const QString lower = name.toLower();
    if (m_sentinelExact.contains(lower))
        return true;
    for (const QString &glob : m_sentinelGlobs) {
        if (globMatch(glob, lower))
            return true;
    }
    return false;
}

bool ExcludeRuleSet::excludesProjectBuildDir(const QString &name) const
{
    return m_excludeBuildDirs && m_buildDirSet.contains(name.toLower());
}

bool ExcludeRuleSet::globMatch(QStringView pattern, QStringView text)
{
    qsizetype p = 0;
    qsizetype t = 0;
    qsizetype star = -1;
    qsizetype mark = 0;

    while (t < text.size()) {
        if (p < pattern.size()
            && (pattern.at(p) == QLatin1Char('?')
                || pattern.at(p).toLower() == text.at(t).toLower())) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern.at(p) == QLatin1Char('*')) {
            star = p++;
            mark = t;
        } else if (star >= 0) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern.at(p) == QLatin1Char('*'))
        ++p;
    return p == pattern.size();
}

ExcludeRuleSet::ProjectPolicy ExcludeRuleSet::projectPolicyFromString(const QString &text)
{
    const QString s = text.trimmed().toLower();
    if (s == QLatin1String("demote"))
        return ProjectPolicy::Demote;
    if (s == QLatin1String("index") || s == QLatin1String("all"))
        return ProjectPolicy::Index;
    return ProjectPolicy::Skip;
}

QString ExcludeRuleSet::projectPolicyToString(ProjectPolicy policy)
{
    switch (policy) {
    case ProjectPolicy::Demote: return QStringLiteral("demote");
    case ProjectPolicy::Index:  return QStringLiteral("index");
    default:                    return QStringLiteral("skip");
    }
}
