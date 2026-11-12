#pragma once

#include <QString>
#include <QStringView>
#include <QVariantList>

#include <cstdint>

struct DownloadRow;

/// 归档规则（治理文档 §2.9，S11）。
///
/// 默认动作是 `suggest`（只给推荐去向，不移动）；`autoOrganize` 显式开启后 `move` 才执行。
/// 求值按 `priority` 升序、**首个命中即止**（first-match-wins）。
class DownloadRules
{
public:
    struct Rule
    {
        QString  name;
        int      priority = 100;
        bool     enabled = true;
        uint16_t kindMask = 0;       ///< 0 = 不限
        qint64   minSize = -1;       ///< -1 = 不限
        qint64   maxSize = -1;
        QString  action = QStringLiteral("suggest");   ///< suggest/move/notify/skip
        QString  dest;               ///< 模板
    };

    struct Decision
    {
        QString action = QStringLiteral("keep");
        QString dest;
        QString ruleName;
        bool matched() const { return action != QLatin1String("keep"); }
    };

    /// 配置格式（`inbox.rules`）：
    /// { name, priority, enabled, kind, size, action, dest }
    void load(const QVariantList &rules);
    Decision evaluate(const DownloadRow &row, const QString &boxPath) const;

    /// `{inbox} {name} {ext} {kind} {yyyy} {MM} {dd} {source}`
    static QString expand(const QString &tpl, const DownloadRow &row,
                          const QString &boxPath);
    static qint64 parseSize(QStringView text);   ///< `>500mb` / `<1k` / `1mb..10mb` → 边界

    int count() const { return static_cast<int>(m_rules.size()); }
    const QList<Rule> &rules() const { return m_rules; }

private:
    QList<Rule> m_rules;
};
