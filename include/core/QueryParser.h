#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

/// 查询语法（A 阶段最小版 + Phase B 治理 token）。
///
/// 语法（治理文档 §1.5）：
///   - 自由词：按空白切分，词与词之间是 **AND**（每个词都要命中文件名）
///   - `-词`：否定项，命中即**排除**
///   - `"多个词"`：短语，作为一个自由词（保留空格）
///   - `:doc` `:img` `:vid` `:aud` `:zip` `:exe` `:code` `:pdf` …：种类过滤（可叠加）
///   - `ext:pdf,pptx`：精确扩展名过滤
///   - `in:downloads` / `in:C:/work`：路径限定（目录名或路径前缀）
///   - `size:>10mb` / `size:<1k` / `size:1mb..10mb`
///   - `date:today` / `yesterday` / `7d` / `>2026-01-01` / `2026-01-01..2026-02-01`
///
/// **非法/未知 token 降级为自由词**（`:` 开头但没匹配到已知种类时整串当普通词），
/// 避免用户输 `:foo` 时结果全空却不知所措。
///
/// 归一化只做一次（整串 `toLower`），下游全部按"已折叠小写"处理。
namespace QueryParser {

struct Parsed
{
    QStringList terms;       ///< 必须全部命中（已折叠小写、已去空白）
    QStringList negations;   ///< 命中任一即排除（已折叠小写，不含前导 '-'）

    bool     hasKind  = false;
    uint16_t kindMask = 0;   ///< 多个 `:kind` 取并集
    QStringList extensions;  ///< 小写、不含点
    QStringList inFilters;   ///< 小写；目录名或路径前缀

    bool   hasSize = false;
    qint64 sizeMin = -1;     ///< -1 = 不设下限
    qint64 sizeMax = -1;     ///< -1 = 不设上限

    bool   hasDate = false;
    qint64 dateAfter  = 0;   ///< Unix 秒；0 = 不设下限
    qint64 dateBefore = 0;   ///< Unix 秒；0 = 不设上限（**不含**）

    bool hasFilters() const
    {
        return hasKind || !extensions.isEmpty() || !inFilters.isEmpty() || hasSize || hasDate;
    }

    bool isEmpty() const { return terms.isEmpty() && negations.isEmpty() && !hasFilters(); }
};

/// 解析原始输入。空串 / 纯空白 → 空的 `Parsed`（调用方据此返回空结果）。
/// 单独的 `-`（后面没有内容）直接忽略。
Parsed parse(const QString &raw);

} // namespace QueryParser
