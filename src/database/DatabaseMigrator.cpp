#include "database/DatabaseMigrator.h"
#include "core/Logger.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <utility>

DatabaseMigrator::DatabaseMigrator(QString databaseLabel, QVector<Step> steps)
    : m_label(std::move(databaseLabel))
    , m_steps(std::move(steps))
{
    std::sort(m_steps.begin(), m_steps.end(),
              [](const Step &a, const Step &b) { return a.version < b.version; });
}

int DatabaseMigrator::currentVersion(QSqlDatabase &db) const
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
        Logger::get().error("DatabaseMigrator[{}]: 读 user_version 失败 — {}",
                            m_label.toStdString(),
                            q.lastError().text().toStdString());
        return -1;
    }
    return q.value(0).toInt();
}

int DatabaseMigrator::latestVersion() const
{
    int latest = 0;
    for (const Step &step : m_steps)
        latest = std::max(latest, step.version);
    return latest;
}

bool DatabaseMigrator::run(QSqlDatabase &db)
{
    const int current = currentVersion(db);
    if (current < 0)
        return false;

    const int latest = latestVersion();

    if (current > latest) {
        // 旧程序打开新库：绝不能继续，否则 UPDATE 语句会静默丢掉它不认识的字段
        Logger::get().error("DatabaseMigrator[{}]: 库是 v{}，本程序只认识到 v{} —— 拒绝启动",
                            m_label.toStdString(), current, latest);
        return false;
    }

    if (current == latest) {
        Logger::get().debug("DatabaseMigrator[{}]: 已是最新 v{}", m_label.toStdString(), latest);
        return true;
    }

    for (const Step &step : m_steps) {
        if (step.version <= current)
            continue;

        if (!db.transaction()) {
            Logger::get().error("DatabaseMigrator[{}]: BEGIN 失败 — {}",
                                m_label.toStdString(),
                                db.lastError().text().toStdString());
            return false;
        }

        bool ok = step.apply && step.apply(db);

        if (ok) {
            // user_version 存在库头里，是**事务性**的：和这一步的 DDL 一起提交/回滚
            QSqlQuery q(db);
            ok = q.exec(QStringLiteral("PRAGMA user_version = %1").arg(step.version));
            if (!ok) {
                Logger::get().error("DatabaseMigrator[{}]: 写 user_version={} 失败 — {}",
                                    m_label.toStdString(), step.version,
                                    q.lastError().text().toStdString());
            }
        }

        if (!ok) {
            db.rollback();
            Logger::get().error("DatabaseMigrator[{}]: v{} 迁移失败（已回滚）— {}",
                                m_label.toStdString(), step.version,
                                step.description.toStdString());
            return false;
        }

        if (!db.commit()) {
            const QString why = db.lastError().text();
            db.rollback();
            Logger::get().error("DatabaseMigrator[{}]: COMMIT v{} 失败 — {}",
                                m_label.toStdString(), step.version, why.toStdString());
            return false;
        }

        Logger::get().info("DatabaseMigrator[{}]: → v{}（{}）",
                           m_label.toStdString(), step.version,
                           step.description.toStdString());
    }

    return true;
}
