#include "ui/InboxBridge.h"

#include "core/FileKind.h"
#include "core/Logger.h"
#include "core/inbox/IngestEngine.h"
#include "core/inbox/TrackingEngine.h"
#include "database/ClipboardDatabase.h"
#include "database/DownloadsDatabase.h"
#include "database/TodoDatabase.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>
#include <QVariantMap>

namespace {

QString humanSize(qint64 bytes)
{
    const double kb = 1024.0;
    if (bytes < kb)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < kb * kb)
        return QStringLiteral("%1 KB").arg(bytes / kb, 0, 'f', 1);
    if (bytes < kb * kb * kb)
        return QStringLiteral("%1 MB").arg(bytes / (kb * kb), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (kb * kb * kb), 0, 'f', 2);
}

QString stateText(const DownloadRow &row)
{
    if (row.pipeState == QLatin1String("Failed"))
        return QStringLiteral("失败");
    if (row.pipeState == QLatin1String("Undone"))
        return QStringLiteral("已撤销");
    if (row.inBox) {
        if (row.dupOf > 0)
            return QStringLiteral("待整理（重复）");
        return QStringLiteral("待整理");
    }
    if (row.trackState == QLatin1String("FiledAway"))
        return QStringLiteral("已归档");
    if (row.trackState == QLatin1String("Deleted"))
        return QStringLiteral("已删除（手动标记）");
    if (row.trackState == QLatin1String("Unknown"))
        return QStringLiteral("已移出（位置未知）");
    return QStringLiteral("已移出");
}

int daysSince(const QString &iso)
{
    if (iso.isEmpty())
        return 0;
    const QDateTime then = QDateTime::fromString(iso, Qt::ISODate);
    if (!then.isValid())
        return 0;
    return static_cast<int>(qMax<qint64>(0, then.date().daysTo(QDate::currentDate())));
}

} // namespace

InboxBridge::InboxBridge(DownloadsDatabase *db, IngestEngine *engine, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_engine(engine)
{
    if (m_engine) {
        connect(m_engine, &IngestEngine::changed, this, &InboxBridge::refresh);
        connect(m_engine, &IngestEngine::notice, this, [this](const QString &text) {
            Q_UNUSED(text);
            refresh();
        });
    }
}

QVariantList InboxBridge::items() const
{
    return buildItems(QStringLiteral("all"));
}

QVariantList InboxBridge::filterItems(const QString &filter) const
{
    return buildItems(filter);
}

QVariantList InboxBridge::buildItems(const QString &filter) const
{
    QVariantList out;
    if (!m_db || !m_db->isOpen())
        return out;

    const QVector<DownloadRow> rows = m_db->list(500);
    for (const DownloadRow &row : rows) {
        if (filter == QLatin1String("inbox") && !row.inBox)
            continue;
        if (filter == QLatin1String("filed") && row.inBox)
            continue;

        QVariantMap item;
        item.insert(QStringLiteral("id"), row.id);
        item.insert(QStringLiteral("name"), row.name);
        item.insert(QStringLiteral("size"), row.size);
        item.insert(QStringLiteral("sizeText"), humanSize(row.size));
        item.insert(QStringLiteral("kind"), FileKind::token(row.catMask));
        item.insert(QStringLiteral("state"), row.pipeState);
        item.insert(QStringLiteral("trackState"), row.trackState);
        item.insert(QStringLiteral("stateText"), stateText(row));
        item.insert(QStringLiteral("inBox"), row.inBox);
        item.insert(QStringLiteral("failed"), row.pipeState == QLatin1String("Failed"));
        item.insert(QStringLiteral("dupOf"), row.dupOf);
        item.insert(QStringLiteral("duplicate"), row.dupOf > 0);
        item.insert(QStringLiteral("suggestDest"), row.suggestDest);
        item.insert(QStringLiteral("ruleName"), row.ruleName);
        item.insert(QStringLiteral("curPath"), row.curPath);
        item.insert(QStringLiteral("boxPath"), row.boxPath);
        item.insert(QStringLiteral("srcPath"), row.srcPath);
        item.insert(QStringLiteral("createdAt"), row.createdAt);
        item.insert(QStringLiteral("fetchedAt"), row.fetchedAt);
        item.insert(QStringLiteral("days"), daysSince(row.fetchedAt.isEmpty()
                                                          ? row.createdAt
                                                          : row.fetchedAt));
        out.append(item);
    }
    return out;
}

int InboxBridge::pendingCount() const
{
    return m_db ? m_db->countInBox() : 0;
}

void InboxBridge::refresh()
{
    emit itemsChanged();
}

QString InboxBridge::pathOf(int id, bool *inBox) const
{
    if (!m_db)
        return QString();
    const auto row = m_db->byId(id);
    if (!row)
        return QString();
    if (inBox)
        *inBox = row->inBox;
    if (!row->curPath.isEmpty() && QFileInfo::exists(row->curPath))
        return row->curPath;
    if (!row->boxPath.isEmpty())
        return row->boxPath;
    return row->curPath;
}

void InboxBridge::openItem(int id)
{
    const QString path = pathOf(id, nullptr);
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void InboxBridge::revealItem(int id)
{
    const QString path = pathOf(id, nullptr);
    if (path.isEmpty())
        return;
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            { QStringLiteral("/select,") + QDir::toNativeSeparators(path) });
}

bool InboxBridge::undoItem(int id)
{
    if (!m_engine)
        return false;
    QString error;
    if (!m_engine->undo(id, &error)) {
        m_lastError = error;
        emit lastErrorChanged();
        Logger::get().warn("InboxBridge: 撤销失败 — {}", error.toStdString());
        return false;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    refresh();
    return true;
}

bool InboxBridge::archiveItem(int id)
{
    if (!m_engine || !m_db)
        return false;
    const auto row = m_db->byId(id);
    if (!row || row->suggestDest.isEmpty()) {
        m_lastError = QStringLiteral("这条记录没有可用的建议去向");
        emit lastErrorChanged();
        return false;
    }

    QString error;
    if (!m_engine->archive(id, row->suggestDest, &error)) {
        m_lastError = error;
        emit lastErrorChanged();
        return false;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    refresh();
    return true;
}

bool InboxBridge::addToTodo(int id)
{
    if (!m_todoDb || !m_db)
        return false;
    const auto row = m_db->byId(id);
    if (!row)
        return false;

    const QString path = row->curPath.isEmpty() ? row->boxPath : row->curPath;
    if (m_todoDb->addTodo(path) < 0) {
        m_lastError = QStringLiteral("加入待办失败");
        emit lastErrorChanged();
        return false;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    return true;
}

bool InboxBridge::copyContent(int id)
{
    if (!m_db)
        return false;
    const QString path = pathOf(id, nullptr);
    if (path.isEmpty())
        return false;

    constexpr qint64 kMaxTextBytes = 1024 * 1024;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxTextBytes) {
        m_lastError = QStringLiteral("文件过大或无法读取（只支持 ≤1MB 文本）");
        emit lastErrorChanged();
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();
    if (raw.contains('\0')) {
        m_lastError = QStringLiteral("二进制文件不复制内容");
        emit lastErrorChanged();
        return false;
    }

    const QString text = QString::fromUtf8(raw);
    if (QClipboard *clipboard = QApplication::clipboard())
        clipboard->setText(text);
    if (m_clipDb)
        m_clipDb->addClip(text, QStringLiteral("text/plain"));

    m_lastError.clear();
    emit lastErrorChanged();
    return true;
}

bool InboxBridge::relocateItem(int id)
{
    if (!m_tracking)
        return false;
    QString error;
    if (!m_tracking->relocate(id, &error)) {
        m_lastError = error;
        emit lastErrorChanged();
        return false;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    refresh();
    return true;
}

bool InboxBridge::markDeleted(int id)
{
    if (!m_db)
        return false;
    const auto row = m_db->byId(id);
    if (!row)
        return false;

    m_db->updateTrackState(id, QStringLiteral("Deleted"),
                           row->curPath.isEmpty() ? row->boxPath : row->curPath,
                           false,
                           QDateTime::currentDateTime().toString(Qt::ISODate));
    m_lastError.clear();
    emit lastErrorChanged();
    refresh();
    return true;
}

bool InboxBridge::copyPath(int id)
{
    const QString path = pathOf(id, nullptr);
    if (path.isEmpty())
        return false;
    if (QClipboard *clipboard = QApplication::clipboard())
        clipboard->setText(QDir::toNativeSeparators(path));
    return true;
}
