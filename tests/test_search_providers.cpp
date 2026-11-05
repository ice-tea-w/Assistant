// A7：AppBridge 的 provider + k-way merge 行为
//
// 不碰文件索引（FileProvider 在 setFileIndex 时才挂上），只验证内置项与计算器
// 两条 provider 的合并顺序，以及计算器的输入门禁。

#include <QCoreApplication>

#include <limits>
#include <QtTest>

#include "ui/AppBridge.h"
#include "ui/SearchResult.h"

class TestSearchProviders : public QObject
{
    Q_OBJECT

private slots:
    void emptyQueryReturnsDefaultList();
    void builtinMatchesByFuzzy();
    void calculatorWinsOnScore();
    void calculatorGatedWithoutOperator();
    void resultsAreSortedByScore();

};

void TestSearchProviders::emptyQueryReturnsDefaultList()
{
    AppBridge bridge;
    bridge.search(QString());
    const auto results = bridge.searchResults();
    QVERIFY(!results.isEmpty());   // 空查询 = 内置命令列表
}

void TestSearchProviders::builtinMatchesByFuzzy()
{
    AppBridge bridge;
    bridge.search(QStringLiteral("notepad"));

    const auto results = bridge.searchResults();
    QVERIFY(!results.isEmpty());
    bool found = false;
    for (const QVariant &v : results) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("title")).toString().contains("Notepad", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestSearchProviders::calculatorWinsOnScore()
{
    AppBridge bridge;
    bridge.search(QStringLiteral("2+2"));

    const auto results = bridge.searchResults();
    QVERIFY(!results.isEmpty());

    const QVariantMap first = results.first().toMap();
    QCOMPARE(first.value(QStringLiteral("type")).toString(), QStringLiteral("calculator"));
    QCOMPARE(first.value(QStringLiteral("subtitle")).toString(), QStringLiteral("= 4"));
}

void TestSearchProviders::calculatorGatedWithoutOperator()
{
    AppBridge bridge;
    bridge.search(QStringLiteral("2026"));   // 有数字、没有运算符

    const auto results = bridge.searchResults();
    for (const QVariant &v : results) {
        QVERIFY(v.toMap().value(QStringLiteral("type")).toString()
                != QStringLiteral("calculator"));
    }
}

void TestSearchProviders::resultsAreSortedByScore()
{
    AppBridge bridge;
    bridge.search(QStringLiteral("open"));

    const auto results = bridge.searchResults();
    QVERIFY(results.size() >= 2);
    int previous = std::numeric_limits<int>::max();
    for (const QVariant &v : results) {
        const int score = v.toMap().value(QStringLiteral("score")).toInt();
        QVERIFY2(score <= previous, "k-way merge 后必须保持 score 降序");
        previous = score;
    }
}

QTEST_GUILESS_MAIN(TestSearchProviders)
#include "test_search_providers.moc"
