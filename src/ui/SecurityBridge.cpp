#include "ui/SecurityBridge.h"

#include "core/Logger.h"
#include "file_guard/file_guard.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <filesystem>

namespace {

QString toQString(const std::filesystem::path &path)
{
    return QString::fromStdWString(path.wstring());
}

std::filesystem::path toPath(const QString &path)
{
    return std::filesystem::path(path.toStdWString());
}

QString humanSize(quint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

} // namespace

SecurityBridge::SecurityBridge(QObject *parent)
    : QObject(parent)
{
    refresh();
    // 演示/截图用：设置环境变量 FG_SECURITY_AUTORUN=1 时启动后自动跑一次体检
    if (qEnvironmentVariableIsSet("FG_SECURITY_AUTORUN"))
        QTimer::singleShot(600, this, &SecurityBridge::runSelfCheck);
}

QString SecurityBridge::dataDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
           + QStringLiteral("/security");
}

// ---------------------------------------------------------------------------
// 单项检查
// ---------------------------------------------------------------------------
bool SecurityBridge::checkDataDir(QString *detail)
{
    try {
        const std::filesystem::path home = toPath(dataDir());
        std::filesystem::create_directories(home);
        const std::filesystem::path probe = home / "health.probe";
        file_guard::util::writeText(probe, "ok");
        std::error_code ignored;
        std::filesystem::remove(probe, ignored);
        *detail = QStringLiteral("可读写：%1").arg(toQString(home));
        return true;
    } catch (const std::exception &ex) {
        *detail = QString::fromUtf8(ex.what());
        return false;
    }
}

bool SecurityBridge::checkAppKey(QString *detail)
{
    try {
        const std::filesystem::path keyFile = toPath(dataDir()) / "app.key";
        if (!std::filesystem::exists(keyFile)) {
            *detail = QStringLiteral("不存在（将在下次检查时生成）");
            return false;
        }
        const auto raw = file_guard::util::readAll(keyFile);
        *detail = raw.size() >= 32 ? QStringLiteral("已就绪（%1 字节，仅本机）").arg(raw.size())
                                   : QStringLiteral("长度异常");
        return raw.size() >= 32;
    } catch (const std::exception &ex) {
        *detail = QString::fromUtf8(ex.what());
        return false;
    }
}

bool SecurityBridge::checkAudit(QString *detail)
{
    try {
        const std::filesystem::path home = toPath(dataDir());
        const auto raw = file_guard::util::readAll(home / "app.key");
        file_guard::audit::AuditLog audit(home / "audit", file_guard::util::hexEncode(raw),
                                          100'000);
        const auto result = audit.verify();
        m_auditOk = result.ok;
        m_auditRecords = result.records;
        m_auditHead = QString::fromStdString(result.head);
        *detail = result.ok
                      ? QStringLiteral("%1 条记录 · 链头 %2")
                            .arg(result.records)
                            .arg(m_auditHead.left(12))
                      : QStringLiteral("校验失败");
        return result.ok;
    } catch (const std::exception &ex) {
        m_auditOk = false;
        *detail = QString::fromUtf8(ex.what());
        return false;
    }
}

bool SecurityBridge::checkBackup(QString *detail)
{
    try {
        const std::filesystem::path home = toPath(dataDir());
        const auto raw = file_guard::util::readAll(home / "app.key");
        file_guard::BackupStore store(home / "backups", file_guard::util::hexEncode(raw),
                                      600'000, 100'000);
        const auto entries = store.list();
        m_backupCount = static_cast<int>(entries.size());
        m_backupBytes = 0;
        for (const auto &entry : entries)
            m_backupBytes += entry.size;
        m_backupOk = store.verify(false).ok;
        *detail = QStringLiteral("%1 个版本 · %2").arg(m_backupCount).arg(humanSize(m_backupBytes));
        return m_backupOk;
    } catch (const std::exception &ex) {
        m_backupOk = false;
        *detail = QString::fromUtf8(ex.what());
        return false;
    }
}

void SecurityBridge::checkVault(QString *detail)
{
    try {
        file_guard::Vault vault(toPath(dataDir()) / "vault", 600'000, 100'000);
        m_vaultExists = vault.exists();
        *detail = m_vaultExists ? QStringLiteral("已初始化（解锁需口令）")
                                : QStringLiteral("未初始化");
    } catch (const std::exception &ex) {
        m_vaultExists = false;
        *detail = QString::fromUtf8(ex.what());
    }
}

// ---------------------------------------------------------------------------
// 启动时静默刷新
// ---------------------------------------------------------------------------
void SecurityBridge::refresh()
{
    if (m_busy)
        return;
    m_busy = true;
    emit changed();

    QString detail;
    checkDataDir(&detail);
    if (!std::filesystem::exists(toPath(dataDir()) / "app.key")) {
        // 首次启动：生成 app.key（与 C++ 模块共享的本地密钥）
        try {
            const auto raw = file_guard::crypto::randomBytes(32);
            file_guard::util::writeAll(toPath(dataDir()) / "app.key", raw);
            file_guard::util::restrictAcl(toPath(dataDir()) / "app.key");
        } catch (...) {
        }
    }
    checkAppKey(&detail);
    checkAudit(&detail);
    checkBackup(&detail);
    checkVault(&detail);

    m_summary = QStringLiteral("安全模块 %1 · 审计 %2 条 · 备份 %3 个 · 保险箱%4")
                    .arg(m_version)
                    .arg(m_auditRecords)
                    .arg(m_backupCount)
                    .arg(m_vaultExists ? QStringLiteral("已初始化") : QStringLiteral("未初始化"));

    Logger::get().info("SecurityBridge(C++): audit_ok={} records={} backups={} vault_exists={}",
                       m_auditOk, m_auditRecords, m_backupCount, m_vaultExists);
    buildCards();
    writeReport();
    m_busy = false;
    emit changed();
}

// ---------------------------------------------------------------------------
// 体检过程（逐步展示）
// ---------------------------------------------------------------------------
void SecurityBridge::startHealthCheck()
{
    if (m_healthRunning)
        return;
    m_healthSteps.clear();
    m_stepIndex = 0;
    m_healthRunning = true;
    emit healthChanged();
    QTimer::singleShot(120, this, &SecurityBridge::runNextStep);
}

void SecurityBridge::runNextStep()
{
    if (!m_healthRunning)
        return;

    QString title;
    QString detail;
    bool ok = true;
    switch (m_stepIndex) {
    case 0:
        title = QStringLiteral("数据目录");
        ok = checkDataDir(&detail);
        break;
    case 1:
        title = QStringLiteral("应用密钥");
        ok = checkAppKey(&detail);
        break;
    case 2:
        title = QStringLiteral("审计链");
        ok = checkAudit(&detail);
        break;
    case 3:
        title = QStringLiteral("备份索引");
        ok = checkBackup(&detail);
        break;
    case 4:
        title = QStringLiteral("保险箱");
        checkVault(&detail);
        ok = true; // 未初始化不是错误
        break;
    default:
        title = QStringLiteral("汇总");
        detail = QStringLiteral("审计 %1 条 · 备份 %2 个 · 保险箱%3")
                     .arg(m_auditRecords)
                     .arg(m_backupCount)
                     .arg(m_vaultExists ? QStringLiteral("已初始化") : QStringLiteral("未初始化"));
        ok = m_auditOk && m_backupOk;
        break;
    }

    appendStep(title, ok ? QStringLiteral("ok") : QStringLiteral("error"), detail);
    ++m_stepIndex;

    if (m_stepIndex >= 6) {
        m_summary = QStringLiteral("安全模块 %1 · 审计 %2 条 · 备份 %3 个 · 保险箱%4")
                        .arg(m_version)
                        .arg(m_auditRecords)
                        .arg(m_backupCount)
                        .arg(m_vaultExists ? QStringLiteral("已初始化") : QStringLiteral("未初始化"));
        buildCards();
        writeReport();
        m_healthRunning = false;
        emit healthChanged();
        emit changed();
        emit toast(QStringLiteral("安全体检完成"));
        return;
    }
    QTimer::singleShot(180, this, &SecurityBridge::runNextStep);
}

void SecurityBridge::appendStep(const QString &title, const QString &state,
                                const QString &detail)
{
    QVariantMap step;
    step.insert(QStringLiteral("title"), title);
    step.insert(QStringLiteral("state"), state);
    step.insert(QStringLiteral("detail"), detail);
    step.insert(QStringLiteral("time"),
                QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    m_healthSteps.append(step);
    emit healthChanged();
}

void SecurityBridge::runSelfCheck()
{
    startHealthCheck();
}

// ---------------------------------------------------------------------------
// 扫描
// ---------------------------------------------------------------------------
void SecurityBridge::runScan(const QString &path)
{
    if (m_busy)
        return;
    QString target = path;
    if (target.isEmpty())
        target = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (target.isEmpty() || !QFileInfo::exists(target)) {
        emit toast(QStringLiteral("路径不存在：%1").arg(target));
        return;
    }

    m_busy = true;
    emit changed();
    emit toast(QStringLiteral("正在扫描：%1").arg(target));
    try {
        const auto report = file_guard::scan::scanPath(toPath(target), true);
        m_scanDetail = QStringLiteral("%1 个文件 · 高 %2 / 中 %3 / 低 %4")
                           .arg(report.filesScanned)
                           .arg(report.high)
                           .arg(report.medium)
                           .arg(report.low);

        // 明细报告（点击「敏感扫描」卡时打开）
        QString scanReport;
        scanReport += QStringLiteral("Assistant 敏感扫描报告\n");
        scanReport += QStringLiteral("扫描目标：%1\n").arg(target);
        scanReport += QStringLiteral("扫描时间：%1\n\n")
                          .arg(QDateTime::currentDateTime().toString(
                              QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        scanReport += QStringLiteral("共 %1 个文件 · 高 %2 / 中 %3 / 低 %4\n\n")
                          .arg(report.filesScanned)
                          .arg(report.high)
                          .arg(report.medium)
                          .arg(report.low);
        for (const auto &file : report.results) {
            for (const auto &finding : file.findings) {
                scanReport += QStringLiteral("%1（行 %2）\n")
                                  .arg(toQString(file.path))
                                  .arg(finding.line);
                scanReport += QStringLiteral("  规则：%1\n")
                                  .arg(QString::fromStdString(finding.rule));
                scanReport += QStringLiteral("  样例：%1\n\n")
                                  .arg(QString::fromStdString(finding.sample));
            }
        }
        if (report.results.empty() || report.high == 0)
            scanReport += QStringLiteral("未发现高置信度敏感信息。\n");
        QFile scanOut(dataDir() + QStringLiteral("/scan-report.txt"));
        if (scanOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            scanOut.write(scanReport.toUtf8());
            scanOut.close();
        }
        try {
            const std::filesystem::path home = toPath(dataDir());
            const auto raw = file_guard::util::readAll(home / "app.key");
            file_guard::audit::AuditLog audit(home / "audit",
                                              file_guard::util::hexEncode(raw), 100'000);
            audit.append("scan", "assistant", target.toStdString());
        } catch (...) {
        }
    } catch (const std::exception &ex) {
        m_scanDetail = QStringLiteral("扫描失败：%1").arg(QString::fromUtf8(ex.what()));
        emit toast(QStringLiteral("扫描失败：%1").arg(QString::fromUtf8(ex.what())));
    }
    m_busy = false;
    buildCards();
    writeReport();
    emit changed();
}

// ---------------------------------------------------------------------------
// 卡片 / 报告
// ---------------------------------------------------------------------------
void SecurityBridge::buildCards()
{
    QVariantList cards;
    const auto addCard = [&cards](const QString &key, const QString &title, const QString &state,
                                  const QString &detail) {
        QVariantMap card;
        card.insert(QStringLiteral("key"), key);
        card.insert(QStringLiteral("title"), title);
        card.insert(QStringLiteral("state"), state);
        card.insert(QStringLiteral("detail"), detail);
        cards.append(card);
    };

    addCard(QStringLiteral("audit"), QStringLiteral("审计链"),
            m_auditOk ? (m_auditRecords > 0 ? QStringLiteral("ok") : QStringLiteral("idle"))
                      : QStringLiteral("error"),
            m_auditRecords > 0
                ? QStringLiteral("%1 条记录 · 链头 %2").arg(m_auditRecords).arg(m_auditHead.left(12))
                : QStringLiteral("暂无记录，操作后自动写入"));

    addCard(QStringLiteral("backup"), QStringLiteral("备份库"),
            m_backupOk ? (m_backupCount > 0 ? QStringLiteral("ok") : QStringLiteral("idle"))
                       : QStringLiteral("error"),
            m_backupCount > 0
                ? QStringLiteral("%1 个版本 · %2").arg(m_backupCount).arg(humanSize(m_backupBytes))
                : QStringLiteral("暂无备份数据"));

    addCard(QStringLiteral("vault"), QStringLiteral("保险箱"),
            m_vaultExists ? QStringLiteral("ok") : QStringLiteral("idle"),
            m_vaultExists ? QStringLiteral("已初始化 · 解锁需口令")
                          : QStringLiteral("未初始化"));

    addCard(QStringLiteral("scan"), QStringLiteral("敏感扫描"),
            m_scanDetail.startsWith(QStringLiteral("扫描失败")) ? QStringLiteral("error")
            : m_scanDetail.startsWith(QStringLiteral("未扫描")) ? QStringLiteral("idle")
                                                                : QStringLiteral("ok"),
            m_scanDetail);

    m_cards = cards;
}

void SecurityBridge::writeReport() const
{
    const QString dir = dataDir();
    QDir().mkpath(dir);
    const QString file = dir + QStringLiteral("/security-report.txt");
    QString text;
    text += QStringLiteral("Assistant 安全中心报告\n");
    text += QStringLiteral("生成时间：%1\n\n")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    text += QStringLiteral("审计链：%1（记录 %2 条，链头 %3）\n")
                .arg(m_auditOk ? QStringLiteral("正常") : QStringLiteral("异常"))
                .arg(m_auditRecords)
                .arg(m_auditHead.isEmpty() ? QStringLiteral("-") : m_auditHead);
    text += QStringLiteral("备份库：%1 个版本，%2（%3）\n")
                .arg(m_backupCount)
                .arg(humanSize(m_backupBytes))
                .arg(m_backupOk ? QStringLiteral("索引校验通过") : QStringLiteral("索引校验失败"));
    text += QStringLiteral("保险箱：%1\n")
                .arg(m_vaultExists ? QStringLiteral("已初始化（解锁需口令）")
                                   : QStringLiteral("未初始化"));
    text += QStringLiteral("敏感扫描：%1\n\n").arg(m_scanDetail);
    text += QStringLiteral("数据目录：%1\n").arg(dir);

    QFile out(file);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
}

void SecurityBridge::openReport()
{
    writeReport();
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(dataDir() + QStringLiteral("/security-report.txt")));
}

void SecurityBridge::openDataDir()
{
    QDir().mkpath(dataDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(dataDir()));
}

void SecurityBridge::openCard(const QString &key)
{
    const QString home = dataDir();
    QDir().mkpath(home);
    const auto openPath = [](const QString &path) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    };

    if (key == QStringLiteral("audit")) {
        QDir().mkpath(home + QStringLiteral("/audit"));
        openPath(home + QStringLiteral("/audit"));
    } else if (key == QStringLiteral("backup")) {
        QDir().mkpath(home + QStringLiteral("/backups"));
        openPath(home + QStringLiteral("/backups"));
    } else if (key == QStringLiteral("vault")) {
        QDir().mkpath(home + QStringLiteral("/vault"));
        openPath(home + QStringLiteral("/vault"));
    } else if (key == QStringLiteral("scan")) {
        const QString scanReport = home + QStringLiteral("/scan-report.txt");
        if (QFileInfo::exists(scanReport))
            openPath(scanReport);
        else
            emit toast(QStringLiteral("还没有扫描记录，先点「扫描下载目录」"));
    }
}
