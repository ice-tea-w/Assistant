#pragma once

#include <QString>
#include <QVector>

#include <functional>

class QSqlDatabase;

/// 前向 schema 迁移（`PRAGMA user_version`，架构文档 §3.6）。
///
/// 规则**写死在实现里**，别绕过：
///  - **只做前向**：`migrate_1_to_2()` / `migrate_2_to_3()` 顺序执行，**每步一个事务**；
///  - **禁止 `DROP TABLE` + 重建**（会丢数据）；加列用 `ALTER TABLE … ADD COLUMN`；
///  - 迁移必须是打开数据库后的**第一个动作**，不能"用到哪个表再顺手建"；
///  - 库的 `user_version` **高于**代码已知版本 → **拒绝**（旧程序打开新库会静默丢字段）。
///
/// 两个 DB 的失败策略不同（§3.6 表格），由**调用方**决定，migrator 只负责如实失败：
///  - `file_index.db`（派生数据）：迁移失败 → 删库重扫；
///  - `downloads.db`（权威数据）：迁移失败 → 拒绝启动 + 保留备份，**绝不静默重建**。
class DatabaseMigrator
{
public:
    struct Step
    {
        int version = 0;                             ///< 跑完这一步 user_version 就是它
        QString description;
        std::function<bool(QSqlDatabase &)> apply;   ///< 返回 false = 失败（本步回滚）
    };

    /// @param databaseLabel 只用于日志（`"file_index"` / `"downloads"`）
    DatabaseMigrator(QString databaseLabel, QVector<Step> steps);

    /// 读 `user_version` → 逐版本迁移。全部成功返回 true；失败的那一步已回滚。
    bool run(QSqlDatabase &db);

    int currentVersion(QSqlDatabase &db) const;
    int latestVersion() const;

private:
    QString m_label;
    QVector<Step> m_steps;   ///< 构造时按 version 升序排好
};
