#pragma once

#include <QList>
#include <QString>

#include "ui/SearchResult.h"

/// 搜索提供者（A7）：每个来源只回答"给定文本，我的 top-N 是什么"。
///
/// 存在理由：A7 之前 `AppBridge::search()` 是"把内置项、文件、应用全部拼成一个大列表
/// 再全排序"，来源越多越慢、也越难让插件贡献结果。拆成 provider 之后：
///  - 每个 provider 自己截断（只返回 top-N），
///  - 组合根做 **k-way merge**，不重复排序整表，
///  - 插件（M4）可以走同一接口贡献检索结果。
///
/// 约定：
///  1. `query()` 只在**非空**查询时被调用；空查询由组合根处理（默认列表）。
///  2. 返回结果按 `score` 降序；同分按 `priority` 降序，顺序稳定。
///  3. **不要**在这里写历史、写日志以外的副作用；provider 必须可被随时调用。
class ISearchProvider
{
public:
    virtual ~ISearchProvider() = default;

    /// 稳定标识，用于日志与调试（如 "builtin" / "files" / "calc"）。
    virtual QString providerId() const = 0;

    /// 返回最多 @p limit 条，按 score 降序。允许返回空列表。
    virtual QList<SearchResult> query(const QString &text, int limit) = 0;
};
