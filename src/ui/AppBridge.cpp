#include "ui/AppBridge.h"
#include "core/FuzzyMatcher.h"
#include "core/Calculator.h"
#include "core/ISearchProvider.h"
#include "core/FileIndex.h"
#include "database/HistoryDatabase.h"
#include "core/Logger.h"

#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QApplication>

#include <algorithm>
#include <memory>
#include <queue>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

    /// 每个 provider 的 top-N 与合并后的总上限。
    constexpr int kPerProviderLimit  = 20;
    constexpr int kMergedResultLimit = 50;

    bool betterFirst(const SearchResult &a, const SearchResult &b)
    {
        if (a.score != b.score)
            return a.score > b.score;
        return a.priority > b.priority;
    }

    /// 内置命令 + 已索引应用：五级模糊匹配，自己截断 top-N。
    class BuiltinProvider : public ISearchProvider
    {
    public:
        explicit BuiltinProvider(const QList<SearchResult> *items) : m_items(items) {}

        QString providerId() const override { return QStringLiteral("builtin"); }

        QList<SearchResult> query(const QString &text, int limit) override
        {
            QList<SearchResult> out;
            for (const SearchResult &item : *m_items) {
                const int score = FuzzyMatcher::score(text, item.title);
                if (score > 0) {
                    SearchResult hit = item;
                    hit.score = score;
                    out.append(hit);
                }
            }
            std::sort(out.begin(), out.end(), betterFirst);
            if (out.size() > limit)
                out = out.mid(0, limit);
            return out;
        }

    private:
        const QList<SearchResult> *m_items;
    };

    /// 计算器：**先门禁再解析**（A7）—— 只在"含数字 + 含运算符 + 长度≥3"时才跑解析器。
    class CalculatorProvider : public ISearchProvider
    {
    public:
        QString providerId() const override { return QStringLiteral("calc"); }

        QList<SearchResult> query(const QString &text, int limit) override
        {
            if (limit <= 0)
                return {};

            const QString q = text.trimmed();
            if (q.length() < 3)
                return {};

            bool hasDigit = false;
            bool hasOp    = false;
            for (const QChar &c : q) {
                if (c.isDigit())
                    hasDigit = true;
                if (c == QLatin1Char('+') || c == QLatin1Char('-')
                    || c == QLatin1Char('*') || c == QLatin1Char('/'))
                    hasOp = true;
            }
            if (!hasDigit || !hasOp)
                return {};

            double val = 0.0;
            QString error;
            if (!Calculator::tryParse(q, &val, &error))
                return {};

            QString resultStr;
            if (std::isfinite(val) && val == static_cast<long long>(val))
                resultStr = QString::number(static_cast<long long>(val));
            else
                resultStr = QString::number(val, 'g', 10);

            SearchResult hit;
            hit.id         = QStringLiteral("calc://") + q;
            hit.title      = q;
            hit.subtitle   = QStringLiteral("= ") + resultStr;
            hit.resultText = hit.subtitle;
            hit.type       = SearchResult::Calculator;
            hit.priority   = 100;
            hit.score      = 1001;   // 高于任何模糊匹配
            return {hit};
        }
    };

    /// 文件：直接转发 FileIndex（v2 是唯一实现，A7 起没有 legacy 分支）。
    class FileProvider : public ISearchProvider
    {
    public:
        explicit FileProvider(FileIndex *index) : m_index(index) {}

        QString providerId() const override { return QStringLiteral("files"); }

        QList<SearchResult> query(const QString &text, int limit) override
        {
            if (!m_index)
                return {};
            return m_index->query(text, limit);
        }

    private:
        FileIndex *m_index;
    };

} // namespace

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

    m_providers.emplace_back(std::make_unique<BuiltinProvider>(&m_allItems));
    m_providers.emplace_back(std::make_unique<CalculatorProvider>());
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
    if (query.isEmpty()) {
        m_searchResults = m_allItems;          // 空查询不跑 provider
    } else {
        QVector<QList<SearchResult>> lists;
        lists.reserve(static_cast<int>(m_providers.size()));
        for (const auto &provider : m_providers)
            lists.append(provider->query(query, kPerProviderLimit));

        m_searchResults = mergeTopK(lists, kMergedResultLimit);
    }

    emit searchResultsChanged();

    if (m_searchResults.isEmpty()) {
        setStatus(QStringLiteral("No matching commands"));
    } else {
        setStatus(QStringLiteral("%1 result(s)").arg(m_searchResults.size()));
    }

    // A2：v2 的索引在后台建，窗口先可用。索引没就绪时把状态说清楚，
    // 免得"搜不到文件"被当成坏了（旧版是同步建完索引才显示窗口）。
    if (m_fileIndex && !m_fileIndex->isReady())
        setStatus(QStringLiteral("正在建立索引…"));

    // 记住这次查询：真正的"搜索历史"在 executeCommand() 里写（见下方说明）。
    m_lastQuery = query.trimmed();

    // A1 修 P1-5：这里以前会对**每一次按键**（≥2 字符）INSERT 一次搜索历史。
    // 后果有两个 —— 每按键一次磁盘事务（WAL），以及历史表被 re/re/pre… 这类前缀垃圾
    // 淹没，真正的意图根本读不出来。现在只写"执行"的那一次。
    if (m_searchResults.isEmpty()) {
        Logger::get().debug("AppBridge::search '{}' → 0 result(s)", query.toStdString());
    } else {
        Logger::get().debug("AppBridge::search '{}' → {} result(s), top: {}",
                            query.toStdString(),
                            m_searchResults.size(),
                            m_searchResults.first().title.toStdString());
    }
}

QList<SearchResult> AppBridge::mergeTopK(const QVector<QList<SearchResult>> &lists, int limit) const
{
    // k-way merge：每个 provider 内部已经按 score 降序，这里只维护 k 个游标的最小堆
    //（按 score/priority 取"更好"的那条）。复杂度 O(N log k)，不需要把全表重排。
    struct Cursor {
        int list = 0;
        int item = 0;
    };
    struct Worse {
        const QVector<QList<SearchResult>> *lists;
        bool operator()(const Cursor &a, const Cursor &b) const
        {
            const SearchResult &x = (*lists)[a.list][a.item];
            const SearchResult &y = (*lists)[b.list][b.item];
            if (x.score != y.score)
                return x.score < y.score;        // 大顶堆：score 大的优先
            if (x.priority != y.priority)
                return x.priority < y.priority;
            return a.list > b.list;              // 来源顺序稳定
        }
    };

    std::priority_queue<Cursor, std::vector<Cursor>, Worse> heap(Worse{&lists});
    for (int i = 0; i < lists.size(); ++i) {
        if (!lists[i].isEmpty())
            heap.push(Cursor{i, 0});
    }

    QList<SearchResult> merged;
    merged.reserve(qMin(limit, 64));
    while (!heap.empty() && merged.size() < limit) {
        const Cursor top = heap.top();
        heap.pop();
        merged.append(lists[top.list][top.item]);
        if (top.item + 1 < lists[top.list].size())
            heap.push(Cursor{top.list, top.item + 1});
    }
    return merged;
}

void AppBridge::executeCommand(int index)
{
    if (index < 0 || index >= m_searchResults.size())
        return;

    const SearchResult &item = m_searchResults.at(index);

    // 搜索历史的落点：只有用户真的执行了某个结果，这次查询才有意义
    // （A1 修 P1-5：原来记录在 search() 里，等于每按键写一次库）。
    auto recordSearchExecution = [this]() {
        if (m_historyDb && m_lastQuery.length() >= 2)
            m_historyDb->recordSearch(m_lastQuery, m_searchResults.size());
    };

    // Calculator results: copy to clipboard
    if (item.type == SearchResult::Calculator) {
        QClipboard *clipboard = QApplication::clipboard();
        if (clipboard) {
            clipboard->setText(item.subtitle.mid(2));  // strip "= " prefix
            setStatus(QStringLiteral("Copied: %1").arg(item.subtitle));
        }
        recordSearchExecution();
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

        if (ok)
            recordSearchExecution();

        setStatus(ok ? QStringLiteral("OK — %1").arg(item.title)
                     : QStringLiteral("Failed — %1").arg(item.title));
        emit commandExecuted(item.title);
        return;
    }

    // Indexed files: open in their default application (ShellExecute under the
    // hood, via QDesktopServices::openUrl with a local-file URL).
    if (item.type == SearchResult::File) {
        const bool ok = QDesktopServices::openUrl(QUrl::fromLocalFile(item.path));
        if (ok)
            recordSearchExecution();

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
    recordSearchExecution();

    emit commandExecuted(item.title);
}

void AppBridge::activate()
{
    m_searchResults = m_allItems;
    emit searchResultsChanged();
    if (m_fileIndex && !m_fileIndex->isReady())
        setStatus(QStringLiteral("正在建立索引…"));
    else
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

void AppBridge::openMainPanel()
{
    emit mainPanelRequested();
}

void AppBridge::setHistoryDatabase(HistoryDatabase *db)
{
    m_historyDb = db;
}

void AppBridge::setFileIndex(FileIndex *index)
{
    m_fileIndex = index;
    if (!m_fileIndex)
        return;

    m_providers.emplace_back(std::make_unique<FileProvider>(index));

    // 后台索引扫完 → 重跑最近一次查询。用户在"正在建立索引…"期间敲的字
    // 不该白敲 —— 这正是前后台分离要换来的东西。没敲过字就刷新一次默认状态，
    // 否则状态栏会一直停在"正在建立索引…"。
    auto refreshFromIndex = [this](int entries, qint64 ms) {
        if (!m_lastQuery.isEmpty())
            search(m_lastQuery);
        else
            activate();
        Logger::get().info("AppBridge: file index refreshed — {} entries ({} ms)", entries, ms);
    };

    connect(m_fileIndex, &FileIndex::indexReady, this, [this, refreshFromIndex](int entries, qint64 ms) {
        Logger::get().info("AppBridge: file index ready — {} entries in {} ms", entries, ms);
        refreshFromIndex(entries, ms);
    });

    // A6：冷启动的"先可用"阶段也要刷新一次 —— 否则从点击热键到扫描结束之间，
    // 用户看到的是空列表，而其实 DB 里已经有上一轮的世界了。
    connect(m_fileIndex, &FileIndex::cacheLoaded, this,
            [this, refreshFromIndex](int entries, qint64 ms) {
        Logger::get().info("AppBridge: file index from cache — {} entries in {} ms", entries, ms);
        refreshFromIndex(entries, ms);
    });
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
        // debug 级别：验收"状态栏确实切到过正在建立索引…"时用得上
        Logger::get().debug("AppBridge::status '{}'", text.toStdString());
        emit statusTextChanged();
    }
}
