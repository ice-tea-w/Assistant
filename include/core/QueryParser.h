#pragma once

#include <QString>
#include <QStringList>

/// A 阶段的**最小查询语法**（`docs/architecture/v0.4-实施清单.md` §A4）：
///
///   - 自由词：按空白切分，词与词之间是 **AND**（每个词都要命中文件名）
///   - `-词`：否定项，命中即**排除**
///
/// 刻意不做（留给 Phase B 的 QueryParser 扩展）：`:doc` / `ext:` / `in:` / `src:` /
/// `size:` / `date:` 这些 token，以及**多词路径匹配**（A 阶段只匹配文件名）。
///
/// 归一化只做一次（整串 `toLower`），下游全部按"已折叠小写"处理 ——
/// 这是修 P1-2（每按键 N 次 `toLower()` 分配）的前提。
namespace QueryParser {

struct Parsed
{
    QStringList terms;       ///< 必须全部命中（已折叠小写、已去空白）
    QStringList negations;   ///< 命中任一即排除（已折叠小写，不含前导 '-'）

    bool isEmpty() const { return terms.isEmpty() && negations.isEmpty(); }
};

/// 解析原始输入。空串 / 纯空白 → 空的 `Parsed`（调用方据此返回空结果）。
/// 单独的 `-`（后面没有内容）直接忽略。
Parsed parse(const QString &raw);

} // namespace QueryParser
