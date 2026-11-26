#include "ui/SecurityBridge.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {

QString humanSize(qint64 bytes)
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
    , m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(30000);
    connect(m_timeout, &QTimer::timeout, this, &SecurityBridge::handleTimeout);

    probeEnvironment();
    buildCards();
}

SecurityBridge::~SecurityBridge()
{
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->kill();
            m_proc->waitForFinished(1000);
        }
    }
}

void SecurityBridge::setRepoRoot(const QString &repoRoot)
{
    if (repoRoot.isEmpty())
        return;
    m_repoRoot = repoRoot;
    probeEnvironment();
    buildCards();
    emit changed();
}

// ---------------------------------------------------------------------------
// 环境探测
// ---------------------------------------------------------------------------

QString SecurityBridge::locateRepoRoot()
{
    const QString envRoot = qEnvironmentVariable("ASSISTANT_REPO_ROOT");
    QStringList candidates;
    if (!envRoot.isEmpty())
        candidates << envRoot;

    // 构建目录通常是 <repo>/build/<config>，从可执行文件向上找。
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6 && !dir.isRoot(); ++i) {
        candidates << dir.absolutePath();
        dir.cdUp();
    }

    for (const QString &candidate : candidates) {
        const QFileInfo module(candidate + QStringLiteral("/tools/file_guard/__init__.py"));
        if (module.isFile())
            return QDir(candidate).absolutePath();
    }
    return {};
}

QString SecurityBridge::locatePython(const QString &repoRoot)
{
    const QString envPython = qEnvironmentVariable("ASSISTANT_PYTHON");
    if (!envPython.isEmpty() && QFileInfo(envPython).isFile())
        return envPython;

    const QStringList localVenvs = {
        repoRoot + QStringLiteral("/.venv/Scripts/python.exe"),
        repoRoot + QStringLiteral("/tools/file_guard/.venv/Scripts/python.exe"),
    };
    for (const QString &path : localVenvs) {
        if (QFileInfo(path).isFile())
            return path;
    }

    const QString python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (!python.isEmpty())
        return python;
    return QStandardPaths::findExecutable(QStringLiteral("python3"));
}

void SecurityBridge::probeEnvironment()
{
    if (m_repoRoot.isEmpty())
        m_repoRoot = locateRepoRoot();
    m_toolsDir = m_repoRoot + QStringLiteral("/tools/file_guard");
    m_python = m_repoRoot.isEmpty() ? QString() : locatePython(m_repoRoot);

    const bool moduleOk = QFileInfo(m_toolsDir + QStringLiteral("/__init__.py")).isFile();
    m_available = !m_python.isEmpty() && moduleOk;

    m_home = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
             + QStringLiteral("/security");
    QDir().mkpath(m_home);

    m_lastReport = m_repoRoot + QStringLiteral("/docs/security/M3-现状与验收.md");

    if (!m_available)
        m_summary = QStringLiteral("未找到 Python 安全模块（tools/file_guard）");
}

// ---------------------------------------------------------------------------
// 探针调度
// ---------------------------------------------------------------------------

void SecurityBridge::refresh()
{
    if (!m_available) {
        probeEnvironment();
        buildCards();
        emit changed();
        return;
    }
    if (m_busy)
        return;
    m_busy = true;
    m_results.clear();
    m_healthDetail.clear();
    emit changed();

    const QVector<Probe> probes = {
        { QStringLiteral("version"), { QStringLiteral("--version") }, false },
        { QStringLiteral("doctor"), { QStringLiteral("doctor") }, true },
        { QStringLiteral("log"), { QStringLiteral("log"), QStringLiteral("verify") }, true },
        { QStringLiteral("backup"), { QStringLiteral("backup"), QStringLiteral("list") }, true },
        { QStringLiteral("vault"), { QStringLiteral("vault"), QStringLiteral("info") }, true },
    };
    enqueue(probes);
}

void SecurityBridge::runSelfCheck()
{
    emit toast(QStringLiteral("正在运行安全体检…"));
    refresh();
}

void SecurityBridge::runScan(const QString &path)
{
    if (!m_available) {
        emit toast(QStringLiteral("未找到 Python 安全模块"));
        return;
    }
    if (m_busy) {
        emit toast(QStringLiteral("上一项检查还在进行"));
        return;
    }
    QString target = path;
    if (target.isEmpty())
        target = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QFileInfo info(target);
    if (!info.exists()) {
        emit toast(QStringLiteral("路径不存在：%1").arg(target));
        return;
    }

    m_busy = true;
    m_scanDetail.clear();
    emit changed();
    emit toast(QStringLiteral("正在扫描：%1").arg(target));

    Probe probe;
    probe.id = QStringLiteral("scan");
    probe.args = { QStringLiteral("scan"), info.absoluteFilePath(),
                   QStringLiteral("--min-risk"), QStringLiteral("medium") };
    enqueue({ probe });
}

void SecurityBridge::enqueue(const QVector<Probe> &probes)
{
    for (const Probe &probe : probes)
        m_queue.enqueue(probe);
    startNext();
}

void SecurityBridge::startNext()
{
    if (m_queue.isEmpty()) {
        m_timeout->stop();
        m_busy = false;
        buildCards();
        emit changed();
        return;
    }

    m_current = m_queue.dequeue();

    if (!m_proc) {
        m_proc = new QProcess(this);
        m_proc->setProcessChannelMode(QProcess::SeparateChannels);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
        m_proc->setProcessEnvironment(env);

        connect(m_proc, &QProcess::finished, this,
                [this](int exitCode, QProcess::ExitStatus status) {
                    if (m_abandon) {
                        m_abandon = false;
                        return;
                    }
                    if (status == QProcess::CrashExit) {
                        m_timeout->stop();
                        startNext();
                        return;
                    }
                    onProbeFinished(exitCode);
                });
        connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                m_timeout->stop();
                startNext();
            }
        });
    }

    QStringList args = { QStringLiteral("-m"), QStringLiteral("file_guard") };
    args += m_current.args;
    if (m_current.json)
        args << QStringLiteral("--json");
    if (m_current.id != QLatin1String("version"))
        args << QStringLiteral("--home") << m_home;

    // `python -m file_guard` 要求 tools/ 在 sys.path 上；工作目录设成
    // tools/file_guard 会变成 "No module named file_guard"（cwd 里只有包内模块）。
    m_proc->setWorkingDirectory(QFileInfo(m_toolsDir).absolutePath());
    m_proc->start(m_python, args);
    m_timeout->start();
}

void SecurityBridge::handleTimeout()
{
    m_abandon = true;
    if (m_proc && m_proc->state() != QProcess::NotRunning)
        m_proc->kill();
    startNext();
}

void SecurityBridge::onProbeFinished(int exitCode)
{
    m_timeout->stop();
    if (!m_proc)
        return;

    const QByteArray out = m_proc->readAllStandardOutput();
    const QByteArray err = m_proc->readAllStandardError();

    if (m_current.id == QLatin1String("version")) {
        const QString text = QString::fromUtf8(out).trimmed();
        const QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!parts.isEmpty())
            m_version = parts.last();
    } else {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(out, &parseError);
        if (parseError.error == QJsonParseError::NoError && !doc.isNull()) {
            if (doc.isObject())
                m_results.insert(m_current.id, doc.object());
            else if (doc.isArray())
                m_results.insert(m_current.id, QJsonObject{ { QStringLiteral("items"), doc.array() } });
        }
        if (!err.isEmpty())
            m_results.insert(m_current.id + QStringLiteral(".stderr"),
                             QJsonObject{ { QStringLiteral("text"), QString::fromUtf8(err) } });
    }

    Q_UNUSED(exitCode);
    startNext();
}

void SecurityBridge::failFast(const QString &message)
{
    m_queue.clear();
    m_busy = false;
    m_summary = message;
    buildCards();
    emit changed();
}

// ---------------------------------------------------------------------------
// 卡片
// ---------------------------------------------------------------------------

void SecurityBridge::buildCards()
{
    QVariantList cards;
    const auto addCard = [&cards](const QString &key, const QString &title,
                                  const QString &state, const QString &detail) {
        QVariantMap card;
        card.insert(QStringLiteral("key"), key);
        card.insert(QStringLiteral("title"), title);
        card.insert(QStringLiteral("state"), state);
        card.insert(QStringLiteral("detail"), detail);
        cards.append(card);
    };

    if (!m_available) {
        const QString detail = QStringLiteral("未找到 Python 模块或解释器；"
                                              "请确认 tools/file_guard 存在（可选 .venv）");
        addCard(QStringLiteral("audit"), QStringLiteral("审计链"), QStringLiteral("empty"), detail);
        addCard(QStringLiteral("backup"), QStringLiteral("备份库"), QStringLiteral("empty"), detail);
        addCard(QStringLiteral("vault"), QStringLiteral("保险箱"), QStringLiteral("empty"), detail);
        addCard(QStringLiteral("scan"), QStringLiteral("敏感扫描"), QStringLiteral("empty"), detail);
        m_cards = cards;
        return;
    }

    // 审计链
    bool auditUninitialized = false;
    {
        QString state = QStringLiteral("empty");
        QString detail = QStringLiteral("尚未校验");
        if (m_results.contains(QStringLiteral("log"))) {
            const QJsonObject log = m_results.value(QStringLiteral("log"));
            const bool ok = log.value(QStringLiteral("ok")).toBool(false);
            const int records = log.value(QStringLiteral("records")).toInt(0);
            const QString head = log.value(QStringLiteral("head")).toString();
            if (ok) {
                state = QStringLiteral("ok");
                detail = QStringLiteral("%1 条记录 · 链头 %2")
                             .arg(records)
                             .arg(head.left(12));
            } else if (log.value(QStringLiteral("error")).toObject()
                           .value(QStringLiteral("type")).toString()
                       == QLatin1String("PasswordError")) {
                // 还没 init / 没给口令：这是"未配置"，不是哈希链损坏，别吓用户
                state = QStringLiteral("empty");
                detail = QStringLiteral("未初始化 · 先运行 init（或设置 FG_PASSWORD）");
                auditUninitialized = true;
            } else {
                state = QStringLiteral("error");
                detail = QStringLiteral("哈希链校验失败");
            }
        }
        addCard(QStringLiteral("audit"), QStringLiteral("审计链"), state, detail);
    }

    // 备份库
    {
        QString state = QStringLiteral("empty");
        QString detail = QStringLiteral("暂无备份");
        const QJsonObject backup = m_results.value(QStringLiteral("backup"));
        if (backup.contains(QStringLiteral("items"))) {
            const QJsonArray items = backup.value(QStringLiteral("items")).toArray();
            qint64 total = 0;
            for (const QJsonValue &value : items)
                total += static_cast<qint64>(value.toObject().value(QStringLiteral("size")).toDouble());
            state = items.isEmpty() ? QStringLiteral("empty") : QStringLiteral("ok");
            detail = QStringLiteral("%1 个文件 · %2")
                         .arg(items.size())
                         .arg(humanSize(total));
        }
        addCard(QStringLiteral("backup"), QStringLiteral("备份库"), state, detail);
    }

    // 保险箱
    {
        QString state = QStringLiteral("empty");
        QString detail = QStringLiteral("未初始化");
        const QJsonObject vault = m_results.value(QStringLiteral("vault"));
        if (!vault.isEmpty()) {
            const int files = vault.value(QStringLiteral("files")).toInt(0);
            const bool dpapi = vault.value(QStringLiteral("dpapi")).toBool(false);
            state = files > 0 ? QStringLiteral("ok") : QStringLiteral("empty");
            detail = QStringLiteral("%1 个文件 · DPAPI %2")
                         .arg(files)
                         .arg(dpapi ? QStringLiteral("已启用") : QStringLiteral("未启用"));
        }
        addCard(QStringLiteral("vault"), QStringLiteral("保险箱"), state, detail);
    }

    // 敏感扫描
    {
        QString state = QStringLiteral("empty");
        QString detail = m_scanDetail.isEmpty() ? QStringLiteral("未扫描（点“扫描下载目录”）")
                                                : m_scanDetail;
        if (m_results.contains(QStringLiteral("scan"))) {
            const QJsonObject scan = m_results.value(QStringLiteral("scan"));
            const QJsonObject summary = scan.value(QStringLiteral("summary")).toObject();
            const int high = summary.value(QStringLiteral("high")).toInt(0);
            const int medium = summary.value(QStringLiteral("medium")).toInt(0);
            const int files = scan.value(QStringLiteral("files")).toArray().size();
            state = (high + medium) > 0 ? QStringLiteral("warn") : QStringLiteral("ok");
            detail = QStringLiteral("%1 个文件 · 高危 %2 / 中危 %3")
                         .arg(files)
                         .arg(high)
                         .arg(medium);
            m_scanDetail = detail;
        }
        addCard(QStringLiteral("scan"), QStringLiteral("敏感扫描"), state, detail);
    }

    // 头部摘要
    m_healthDetail.clear();
    if (auditUninitialized) {
        m_healthDetail = QStringLiteral("未初始化");
    } else if (m_results.contains(QStringLiteral("doctor"))) {
        const QJsonObject doctor = m_results.value(QStringLiteral("doctor"));
        const bool ok = doctor.value(QStringLiteral("ok")).toBool(false);
        const QJsonArray checks = doctor.value(QStringLiteral("checks")).toArray();
        m_healthDetail = ok ? QStringLiteral("体检通过 %1 项").arg(checks.size())
                            : QStringLiteral("体检有异常");
    }
    QString summary = QStringLiteral("安全模块 v%1").arg(m_version.isEmpty() ? QStringLiteral("?") : m_version);
    if (!m_healthDetail.isEmpty())
        summary += QStringLiteral(" · ") + m_healthDetail;
    // 未初始化时不再重复"审计 未初始化…"（卡片里已有说明），避免头部被截断。
    const QVariantMap auditCard = cards.value(0).toMap();
    if (auditCard.value(QStringLiteral("state")).toString() != QLatin1String("empty"))
        summary += QStringLiteral(" · 审计 %1").arg(auditCard.value(QStringLiteral("detail")).toString());
    m_summary = summary;
    m_cards = cards;
}

void SecurityBridge::openReport()
{
    if (QFileInfo::exists(m_lastReport)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastReport));
        return;
    }
    emit toast(QStringLiteral("未找到验收报告：docs/security/M3-现状与验收.md"));
}

void SecurityBridge::openDataDir()
{
    QDir().mkpath(m_home);
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_home));
}
