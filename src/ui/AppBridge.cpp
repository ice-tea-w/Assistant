#include "ui/AppBridge.h"
#include "core/FuzzyMatcher.h"
#include "core/Calculator.h"
#include "core/FileSearcher.h"
#include "database/HistoryDatabase.h"
#include "core/Logger.h"

#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QApplication>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
    /// Cap on file matches appended per search — files compete with builtins and
    /// apps for screen space, and this keeps the list responsive on broad queries.
    constexpr int kMaxFileResults = 20;
}

AppBridge::AppBridge(QObject *parent)
    : QObject(parent)
{
    using T = SearchResult::Type;

    m_allItems = {
        { QStringLiteral("cmd://opencalculator"),   QStringLiteral("Open Calculator"),
          QStringLiteral("Launch Calculator"),       T::Builtin, 10 },
        { QStringLiteral("cmd://opennotepad"),      QStringLiteral("Open Notepad"),
          QStringLiteral("Text Editor"),             T::App,     10 },
        { QStringLiteral("cmd://fileexplorer"),     QStringLiteral("Open File Explorer"),
          QStringLiteral("Browse files"),            T::Builtin, 10 },
        { QStringLiteral("cmd://cmd"),              QStringLiteral("Command Prompt"),
          QStringLiteral("Windows Terminal"),        T::App,      9 },
        { QStringLiteral("cmd://opensettings"),     QStringLiteral("Open Settings"),
          QStringLiteral("Windows Settings"),        T::Builtin,  9 },
        { QStringLiteral("cmd://searchgoogle"),     QStringLiteral("Search Google"),
          QStringLiteral("Open in browser"),         T::Url,      8 },
        { QStringLiteral("cmd://sysinfo"),          QStringLiteral("System Information"),
          QStringLiteral("msinfo32"),                T::Builtin,  8 },
        { QStringLiteral("cmd://screenshot"),       QStringLiteral("Take Screenshot"),
          QStringLiteral("Snipping Tool"),           T::Builtin,  7 },
        { QStringLiteral("cmd://lockscreen"),       QStringLiteral("Lock Screen"),
          QStringLiteral("Lock workstation"),        T::Builtin,  7 },
        { QStringLiteral("cmd://emptyrecyclebin"),  QStringLiteral("Empty Recycle Bin"),
          QStringLiteral("Clear trash"),             T::Builtin,  6 },
    };

    m_searchResults = m_allItems;
}

QVariantList AppBridge::searchResults() const
{
    QVariantList list;
    list.reserve(m_searchResults.size());
    for (const auto &item : m_searchResults)
        list.append(item.toMap());
    return list;
}

QString AppBridge::statusText() const
{
    return m_statusText;
}

bool AppBridge::isVisible() const
{
    return m_visible;
}

void AppBridge::setVisible(bool visible)
{
    if (m_visible != visible) {
        m_visible = visible;
        emit visibleChanged();
    }
}

void AppBridge::search(const QString &query)
{
    m_searchResults.clear();

    if (query.isEmpty()) {
        m_searchResults = m_allItems;
    } else {
        for (const auto &item : m_allItems) {
            int s = FuzzyMatcher::score(query, item.title);
            if (s > 0) {
                SearchResult matched = item;
                matched.score = s;
                m_searchResults.append(matched);
            }
        }

        // ---- file search (Step 6) ----
        // Files only appear when actively searching; they are NOT part of
        // m_allItems, otherwise the empty-query default list would be huge.
        {
            const QList<FileSearcher::Entry> files =
                FileSearcher::search(query, kMaxFileResults);
            for (const FileSearcher::Entry &f : files) {
                SearchResult item;
                item.id       = QStringLiteral("file://") + f.path;
                item.title    = f.name;
                item.subtitle = f.path;
                item.type     = SearchResult::File;
                item.priority = 1;   // below builtins/apps on score ties
                item.score    = f.score;
                item.path     = f.path;
                m_searchResults.append(item);
            }
        }

        // Sort: highest score first, then highest priority for ties
        std::sort(m_searchResults.begin(), m_searchResults.end(),
                  [](const SearchResult &a, const SearchResult &b) {
                      if (a.score != b.score)
                          return a.score > b.score;
                      return a.priority > b.priority;
                  });

        // ---- calculator detection ----
        // Heuristic: the query must contain at least one digit AND one operator.
        {
            const QString q = query.trimmed();
            bool hasDigit = false;
            bool hasOp    = false;
            for (const QChar &c : q) {
                if (c.isDigit())               hasDigit = true;
                if (c == QLatin1Char('+') || c == QLatin1Char('-') ||
                    c == QLatin1Char('*') || c == QLatin1Char('/'))
                    hasOp = true;
            }

            if (hasDigit && hasOp && q.length() >= 3) {
                double val = 0.0;
                QString err;
                if (Calculator::tryParse(q, &val, &err)) {
                    // Format result — strip trailing zeros for whole numbers
                    QString resultStr;
                    if (val == static_cast<long long>(val) && std::isfinite(val))
                        resultStr = QString::number(static_cast<long long>(val));
                    else
                        resultStr = QString::number(val, 'g', 10);

                    SearchResult calcResult;
                    calcResult.id         = QStringLiteral("calc://") + q;
                    calcResult.title      = q;
                    calcResult.subtitle   = QStringLiteral("= ") + resultStr;
                    calcResult.resultText = QStringLiteral("= ") + resultStr;
                    calcResult.type       = SearchResult::Calculator;
                    calcResult.priority   = 100;
                    calcResult.score      = 1001;  // above all fuzzy matches

                    m_searchResults.prepend(calcResult);
                }
            }
        }
    }

    emit searchResultsChanged();

    if (m_searchResults.isEmpty()) {
        setStatus(QStringLiteral("No matching commands"));
    } else {
        setStatus(QStringLiteral("%1 result(s)").arg(m_searchResults.size()));
    }

    // Record search history (only meaningful queries, not empty/whitespace)
    if (m_historyDb && query.trimmed().length() >= 2) {
        m_historyDb->recordSearch(query.trimmed(), m_searchResults.size());
    }
}

void AppBridge::executeCommand(int index)
{
    if (index < 0 || index >= m_searchResults.size())
        return;

    const SearchResult &item = m_searchResults.at(index);

    // Calculator results: copy to clipboard
    if (item.type == SearchResult::Calculator) {
        QClipboard *clipboard = QApplication::clipboard();
        if (clipboard) {
            clipboard->setText(item.subtitle.mid(2));  // strip "= " prefix
            setStatus(QStringLiteral("Copied: %1").arg(item.subtitle));
        }
        emit commandExecuted(item.title);
        return;
    }

    // Indexed applications (id prefix "app://"): launch by resolved path.
    if (item.id.startsWith(QStringLiteral("app://"))) {
        bool ok = QDesktopServices::openUrl(QUrl::fromLocalFile(item.path));
        if (!ok)
            ok = QProcess::startDetached(item.path);

        if (m_historyDb)
            m_historyDb->recordAppLaunch(item.title, item.path);

        setStatus(ok ? QStringLiteral("OK — %1").arg(item.title)
                     : QStringLiteral("Failed — %1").arg(item.title));
        emit commandExecuted(item.title);
        return;
    }

    // Indexed files: open in their default application (ShellExecute under the
    // hood, via QDesktopServices::openUrl with a local-file URL).
    if (item.type == SearchResult::File) {
        const bool ok = QDesktopServices::openUrl(QUrl::fromLocalFile(item.path));
        setStatus(ok ? QStringLiteral("OK — %1").arg(item.title)
                     : QStringLiteral("Failed — %1").arg(item.title));
        emit commandExecuted(item.title);
        return;
    }

    executeAction(item);

    // Record execution in history
    if (m_historyDb) {
        m_historyDb->recordCommandExecution(item.id, item.title);
    }

    emit commandExecuted(item.title);
}

void AppBridge::activate()
{
    m_searchResults = m_allItems;
    emit searchResultsChanged();
    setStatus(QStringLiteral("Ready — type to search"));
}

void AppBridge::hideWindow()
{
    setVisible(false);
}

void AppBridge::toggleWindow()
{
    setVisible(!m_visible);
}

void AppBridge::setHistoryDatabase(HistoryDatabase *db)
{
    m_historyDb = db;
}

void AppBridge::addApp(const QString &name, const QString &path)
{
    SearchResult item;
    item.id       = QStringLiteral("app://") + path;
    item.title    = name;
    item.subtitle = path;
    item.type     = SearchResult::App;
    item.priority = 5;   // below builtins on ties; fuzzy score dominates on match
    item.path     = path;

    m_allItems.append(item);
    m_searchResults.append(item);
}

// ---- command dispatch (ID-based) ----

void AppBridge::executeAction(const SearchResult &item)
{
    bool ok = false;
    const QString &id = item.id;

    if (id == QStringLiteral("cmd://opencalculator")) {
        ok = QProcess::startDetached("calc.exe");
    } else if (id == QStringLiteral("cmd://opennotepad")) {
        ok = QProcess::startDetached("notepad.exe");
    } else if (id == QStringLiteral("cmd://fileexplorer")) {
        ok = QProcess::startDetached("explorer.exe");
    } else if (id == QStringLiteral("cmd://cmd")) {
        ok = QProcess::startDetached("cmd.exe");
    } else if (id == QStringLiteral("cmd://opensettings")) {
        ok = QProcess::startDetached("ms-settings:");
    } else if (id == QStringLiteral("cmd://searchgoogle")) {
        ok = QDesktopServices::openUrl(QUrl("https://www.google.com"));
    } else if (id == QStringLiteral("cmd://sysinfo")) {
        ok = QProcess::startDetached("msinfo32.exe");
    } else if (id == QStringLiteral("cmd://screenshot")) {
        ok = QProcess::startDetached("explorer.exe",
                QStringList() << "ms-screenclip:");
    }
#ifdef Q_OS_WIN
    else if (id == QStringLiteral("cmd://lockscreen")) {
        ok = LockWorkStation() != 0;
    } else if (id == QStringLiteral("cmd://emptyrecyclebin")) {
        SHEmptyRecycleBin(nullptr, nullptr, 0);
        ok = true;
    }
#endif

    if (ok) {
        setStatus(QStringLiteral("OK — %1").arg(item.title));
    } else {
        setStatus(QStringLiteral("Failed — %1").arg(item.title));
    }
}

void AppBridge::setStatus(const QString &text)
{
    if (m_statusText != text) {
        m_statusText = text;
        emit statusTextChanged();
    }
}
