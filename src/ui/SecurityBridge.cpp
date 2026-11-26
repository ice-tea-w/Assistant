#include "ui/SecurityBridge.h"

#include "core/Logger.h"
#include "file_guard/file_guard.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
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
}

QString SecurityBridge::dataDir() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return base + QStringLiteral("/security");
}

void SecurityBridge::refresh()
{
    if (m_busy)
        return;
    m_busy = true;
    emit changed();

    try {
        const std::filesystem::path home = toPath(dataDir());
        std::filesystem::create_directories(home);

        // 应用本地密钥（随机 32B，只用于本机审计/备份索引，不是用户口令）
        const std::filesystem::path keyFile = home / "app.key";
        std::string key;
        if (std::filesystem::exists(keyFile)) {
            const auto raw = file_guard::util::readAll(keyFile);
            if (raw.size() >= 32)
                key = file_guard::util::hexEncode(raw);
        }
        if (key.empty()) {
            const auto raw = file_guard::crypto::randomBytes(32);
            file_guard::util::writeAll(keyFile, raw);
            file_guard::util::restrictAcl(keyFile);
            key = file_guard::util::hexEncode(raw);
        }

        // 审计链
        try {
            file_guard::audit::AuditLog audit(home / "audit", key, 100'000);
            const auto result = audit.verify();
            m_auditOk = result.ok;
            m_auditRecords = result.records;
            m_auditHead = QString::fromStdString(result.head);
        } catch (const std::exception &ex) {
            m_auditOk = false;
            m_auditRecords = 0;
            Logger::get().warn("SecurityBridge: audit verify failed — {}", ex.what());
        }

        // 备份库
        try {
            file_guard::BackupStore store(home / "backups", key, 600'000, 100'000);
            const auto entries = store.list();
            m_backupCount = static_cast<int>(entries.size());
            m_backupBytes = 0;
            for (const auto &entry : entries)
                m_backupBytes += entry.size;
            m_backupOk = store.verify(false).ok;
        } catch (const std::exception &ex) {
            m_backupOk = false;
            Logger::get().warn("SecurityBridge: backup verify failed — {}", ex.what());
        }

        // 保险箱（只查状态；解锁需要用户口令，不在启动时做）
        try {
            file_guard::Vault vault(home / "vault", 600'000, 100'000);
            m_vaultExists = vault.exists();
        } catch (...) {
            m_vaultExists = false;
        }

        m_summary = QStringLiteral("C++ 安全模块 v%1 · 审计 %2 条 · 备份 %3 个 · 保险箱%4")
                        .arg(m_version)
                        .arg(m_auditRecords)
                        .arg(m_backupCount)
                        .arg(m_vaultExists ? QStringLiteral("已初始化") : QStringLiteral("未初始化"));

        Logger::get().info("SecurityBridge(C++): audit_ok={} records={} backups={} vault_exists={}",
                           m_auditOk, m_auditRecords, m_backupCount, m_vaultExists);
    } catch (const std::exception &ex) {
        m_summary = QStringLiteral("安全模块异常：%1").arg(QString::fromUtf8(ex.what()));
        Logger::get().error("SecurityBridge: {}", ex.what());
    }

    buildCards();
    writeReport();
    m_busy = false;
    emit changed();
}

void SecurityBridge::runSelfCheck()
{
    emit toast(QStringLiteral("正在运行安全体检…"));
    refresh();
}

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

        // 扫描事件写入 C++ 审计链
        try {
            const std::filesystem::path home = toPath(dataDir());
            const std::filesystem::path keyFile = home / "app.key";
            const auto raw = file_guard::util::readAll(keyFile);
            const std::string key = file_guard::util::hexEncode(raw);
            file_guard::audit::AuditLog audit(home / "audit", key, 100'000);
            audit.append("scan", "assistant", target.toStdString());
        } catch (...) {
            // 审计写入失败不影响扫描结果展示
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
            m_auditOk ? QStringLiteral("ok") : QStringLiteral("error"),
            m_auditOk ? QStringLiteral("%1 条记录 · 链头 %2")
                            .arg(m_auditRecords)
                            .arg(m_auditHead.left(12))
                      : QStringLiteral("审计链校验失败"));

    addCard(QStringLiteral("backup"), QStringLiteral("备份库"),
            m_backupOk ? (m_backupCount > 0 ? QStringLiteral("ok") : QStringLiteral("empty"))
                       : QStringLiteral("error"),
            QStringLiteral("%1 个版本 · %2")
                .arg(m_backupCount)
                .arg(humanSize(m_backupBytes)));

    addCard(QStringLiteral("vault"), QStringLiteral("保险箱"),
            m_vaultExists ? QStringLiteral("ok") : QStringLiteral("empty"),
            m_vaultExists ? QStringLiteral("已初始化（解锁需口令）")
                          : QStringLiteral("未初始化"));

    addCard(QStringLiteral("scan"), QStringLiteral("敏感扫描"),
            m_scanDetail.startsWith(QStringLiteral("未扫描")) ? QStringLiteral("empty")
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
    text += QStringLiteral("Assistant 安全中心报告（C++ 安全模块 %1）\n").arg(m_version);
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
                .arg(m_vaultExists ? QStringLiteral("已初始化（解锁需用户口令）")
                                   : QStringLiteral("未初始化"));
    text += QStringLiteral("敏感扫描：%1\n\n").arg(m_scanDetail);
    text += QStringLiteral("数据目录：%1\n").arg(dir);
    text += QStringLiteral("说明：安全能力由 C++ 模块直接提供（无 Python 依赖）。\n");

    QFile out(file);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
}

void SecurityBridge::openReport()
{
    writeReport();
    const QString file = dataDir() + QStringLiteral("/security-report.txt");
    QDesktopServices::openUrl(QUrl::fromLocalFile(file));
}

void SecurityBridge::openDataDir()
{
    const QString dir = dataDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}
