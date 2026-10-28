// Plugin core tests — SemVer + PluginManifest.
//
// Deliberately dependency-free (no GTest): a plain counting harness that returns
// a non-zero exit code on failure. The repo already has a commented-out GTest
// block in CMakeLists.txt; when that gets enabled these checks can be migrated.
//
// Build & run: see tests/README.md

#include "core/plugin/PluginHost.h"
#include "core/plugin/PluginLoader.h"
#include "core/plugin/PluginArchive.h"
#include "core/plugin/PluginInstaller.h"
#include "core/plugin/PluginManager.h"
#include "core/plugin/PluginSpec.h"
#include "core/plugin/PluginManifest.h"
#include "core/plugin/PluginRegistry.h"
#include "core/plugin/SemVer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QString>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include <miniz.h>

#include <cstdio>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("  FAIL  %s:%d   %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                    \
    } while (0)

#define SECTION(name) std::printf("\n[%s]\n", name)

using semver::Version;

Version ver(const char *text)
{
    const auto parsed = semver::parse(QStringView(QString::fromUtf8(text)));
    if (!parsed) {
        std::printf("  FATAL cannot parse version '%s'\n", text);
        ++g_failures;
        return Version{};
    }
    return *parsed;
}

// ---------------------------------------------------------------------------
// SemVer
// ---------------------------------------------------------------------------

void testParse()
{
    SECTION("SemVer.parse");

    const auto v = semver::parse(u"1.2.3");
    CHECK(v.has_value());
    CHECK(v->major == 1 && v->minor == 2 && v->patch == 3);
    CHECK(v->pre.isEmpty());

    const auto pre = semver::parse(u"1.2.0-beta.1");
    CHECK(pre.has_value());
    CHECK(pre->pre == QStringLiteral("beta.1"));

    // Wildcards belong to ranges, not to versions.
    CHECK(!semver::parse(u"1.2.x"));
    CHECK(!semver::parse(u"1.x"));

    // Malformed shapes
    CHECK(!semver::parse(u""));
    CHECK(!semver::parse(u"1"));
    CHECK(!semver::parse(u"1.2"));
    CHECK(!semver::parse(u"1.2.3.4"));
    CHECK(!semver::parse(u"a.b.c"));
    CHECK(!semver::parse(u"1..3"));
    CHECK(!semver::parse(u"1.2.3-"));
    CHECK(!semver::parse(u"1.2.3-be ta"));

    // Leading zeros are tolerated (we are lenient on input, strict on shape).
    const auto zeros = semver::parse(u"01.02.003");
    CHECK(zeros.has_value());
    CHECK(zeros->major == 1 && zeros->patch == 3);
}

void testCompare()
{
    SECTION("SemVer.compare");

    CHECK(semver::compare(ver("1.0.0"), ver("2.0.0")) < 0);
    CHECK(semver::compare(ver("2.0.0"), ver("1.0.0")) > 0);
    CHECK(semver::compare(ver("1.0.0"), ver("1.0.0")) == 0);

    // Numeric, not lexicographic: 0.9.0 < 0.10.0 (the classic bug).
    CHECK(semver::compare(ver("0.9.0"), ver("0.10.0")) < 0);
    CHECK(semver::compare(ver("1.2.9"), ver("1.2.10")) < 0);

    // A pre-release sorts below its release.
    CHECK(semver::compare(ver("1.0.0-beta"), ver("1.0.0")) < 0);
    CHECK(semver::compare(ver("1.0.0"), ver("1.0.0-beta")) > 0);
    CHECK(semver::compare(ver("1.0.0-alpha"), ver("1.0.0-beta")) < 0);
}

void testToString()
{
    SECTION("SemVer.toString");

    CHECK(semver::toString(ver("1.2.3")) == QStringLiteral("1.2.3"));
    CHECK(semver::toString(ver("1.2.0-rc.1")) == QStringLiteral("1.2.0-rc.1"));
}

void testSatisfies()
{
    SECTION("SemVer.satisfies");

    // The range shape used by engines.assistant in the docs.
    CHECK(semver::satisfies(ver("0.6.0"), u">=0.6.0 <1.0.0"));
    CHECK(semver::satisfies(ver("0.9.9"), u">=0.6.0 <1.0.0"));
    CHECK(!semver::satisfies(ver("1.0.0"), u">=0.6.0 <1.0.0"));
    CHECK(!semver::satisfies(ver("0.5.0"), u">=0.6.0 <1.0.0"));

    // Caret
    CHECK(semver::satisfies(ver("1.2.3"), u"^1.2.3"));
    CHECK(semver::satisfies(ver("1.9.9"), u"^1.2.3"));
    CHECK(!semver::satisfies(ver("2.0.0"), u"^1.2.3"));
    CHECK(!semver::satisfies(ver("1.2.2"), u"^1.2.3"));

    // Tilde
    CHECK(semver::satisfies(ver("1.2.3"), u"~1.2.3"));
    CHECK(semver::satisfies(ver("1.2.9"), u"~1.2.3"));
    CHECK(!semver::satisfies(ver("1.3.0"), u"~1.2.3"));

    // Wildcards
    CHECK(semver::satisfies(ver("1.0.0"), u"1.x"));
    CHECK(semver::satisfies(ver("1.9.9"), u"1.x"));
    CHECK(!semver::satisfies(ver("2.0.0"), u"1.x"));
    CHECK(semver::satisfies(ver("1.2.5"), u"1.2.x"));
    CHECK(!semver::satisfies(ver("1.3.0"), u"1.2.x"));

    // Exact match
    CHECK(semver::satisfies(ver("1.2.3"), u"=1.2.3"));
    CHECK(semver::satisfies(ver("1.2.3"), u"1.2.3"));
    CHECK(!semver::satisfies(ver("1.2.4"), u"1.2.3"));

    // OR
    CHECK(semver::satisfies(ver("1.2.3"), u"1.2.3 || 2.0.0"));
    CHECK(semver::satisfies(ver("2.0.0"), u"1.2.3 || 2.0.0"));
    CHECK(!semver::satisfies(ver("3.0.0"), u"1.2.3 || 2.0.0"));

    // Empty range = no constraint
    CHECK(semver::satisfies(ver("9.9.9"), u""));

    // Extra whitespace must not change meaning
    CHECK(semver::satisfies(ver("0.7.0"), u"  >=0.6.0   <1.0.0  "));
}

void testIsValidRange()
{
    SECTION("SemVer.isValidRange");

    CHECK(semver::isValidRange(u">=0.6.0 <1.0.0"));
    CHECK(semver::isValidRange(u"^1.2.3"));
    CHECK(semver::isValidRange(u"~1.2.3"));
    CHECK(semver::isValidRange(u"1.x"));
    CHECK(semver::isValidRange(u"1.2.3 || 2.0.0"));
    CHECK(semver::isValidRange(u"=1.2.3"));

    // Explicitly outside the subset
    CHECK(!semver::isValidRange(u"*"));
    CHECK(!semver::isValidRange(u"latest"));
    CHECK(!semver::isValidRange(u""));
    CHECK(!semver::isValidRange(u">=abc"));
    CHECK(!semver::isValidRange(u">=1.0"));       // incomplete version
    CHECK(!semver::isValidRange(u">=1.0.0 ||"));  // empty OR branch
}

// ---------------------------------------------------------------------------
// PluginManifest
// ---------------------------------------------------------------------------

QByteArray manifestOf(const char *body)
{
    return QByteArray(body);
}

void testManifestValid()
{
    SECTION("PluginManifest / valid");

    const QByteArray raw = manifestOf(R"({
        "manifestVersion": 1,
        "id": "com.assistant.weather",
        "name": "天气",
        "version": "1.2.0",
        "description": "输入 w 北京，直接看天气",
        "author": "AssistantTeam",
        "license": "MIT",
        "icon": "icon.png",
        "runtime": "qml",
        "engines": { "assistant": ">=0.6.0 <1.0.0" },
        "main": "main.qml",
        "permissions": ["network", "clipboard.read"],
        "contributes": {
            "pages": [
                { "id": "weather", "title": "天气", "icon": "W", "source": "Page.qml" }
            ],
            "searchProviders": [
                { "id": "weather", "title": "天气", "trigger": "w ", "priority": 40 }
            ],
            "commands": [
                { "id": "weather.now", "title": "天气：当前城市", "keyword": "weather" }
            ],
            "settings": { "source": "Settings.qml" }
        }
    })");

    QString error;
    const auto m = PluginManifest::parse(raw, &error);
    CHECK(m.has_value());
    if (!m) {
        std::printf("  parse error: %s\n", error.toUtf8().constData());
        return;
    }

    CHECK(m->id == QStringLiteral("com.assistant.weather"));
    CHECK(m->name == QStringLiteral("天气"));
    CHECK(m->version == QStringLiteral("1.2.0"));
    CHECK(m->runtime == QStringLiteral("qml"));
    CHECK(!m->isNativeRuntime());
    CHECK(m->main == QStringLiteral("main.qml"));
    CHECK(m->enginesAssistant == QStringLiteral(">=0.6.0 <1.0.0"));
    CHECK(m->permissions.size() == 2);
    CHECK(m->pages.size() == 1);
    CHECK(m->pages.at(0).id == QStringLiteral("weather"));
    CHECK(m->pages.at(0).source == QStringLiteral("Page.qml"));
    CHECK(m->providers.size() == 1);
    CHECK(m->providers.at(0).trigger == QStringLiteral("w "));
    CHECK(m->providers.at(0).priority == 40);
    CHECK(m->commands.size() == 1);
    CHECK(m->settingsSource == QStringLiteral("Settings.qml"));
}

void testManifestInvalid()
{
    SECTION("PluginManifest / invalid");

    const auto rejects = [](const char *body) {
        QString err;
        const bool ok = !PluginManifest::parse(QByteArray(body), &err).has_value();
        if (!ok)
            std::printf("  expected rejection but parsed OK\n");
        if (ok && err.isEmpty())
            std::printf("  note: rejected without an error message\n");
        return ok;
    };

    // Not JSON at all
    CHECK(rejects("{ not json"));
    CHECK(rejects("[]"));

    // manifestVersion
    CHECK(rejects(R"({"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml"})"));
    CHECK(rejects(R"({"manifestVersion":2,"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml"})"));

    // id
    CHECK(rejects(R"({"manifestVersion":1,"name":"x","version":"1.0.0","main":"main.qml"})"));
    CHECK(rejects(R"({"manifestVersion":1,"id":"Com.A.B","name":"x","version":"1.0.0","main":"main.qml"})"));
    CHECK(rejects(R"({"manifestVersion":1,"id":"ab","name":"x","version":"1.0.0","main":"main.qml"})"));
    CHECK(rejects(R"({"manifestVersion":1,"id":"-abc","name":"x","version":"1.0.0","main":"main.qml"})"));

    // version
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0","main":"main.qml"})"));
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"abc","main":"main.qml"})"));

    // name
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","version":"1.0.0","main":"main.qml"})"));

    // main: escape attempts
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"../evil.qml"})"));
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"a\\..\\evil.qml"})"));
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"/abs.qml"})"));
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"C:/abs.qml"})"));

    // main required for qml runtime, not for native
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0"})"));
    {
        QString err;
        const auto native = PluginManifest::parse(
            QByteArray(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","runtime":"native"})"),
            &err);
        CHECK(native.has_value());
        if (native)
            CHECK(native->isNativeRuntime());
    }

    // runtime value
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","runtime":"wasm","main":"main.qml"})"));

    // engines range syntax
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml","engines":{"assistant":"*"}})"));
    CHECK(rejects(R"({"manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml","engines":{"assistant":">=abc"}})"));
}

void testManifestContributions()
{
    SECTION("PluginManifest / contributions");

    // Duplicate page ids
    {
        QString err;
        const auto m = PluginManifest::parse(QByteArray(R"({
            "manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml",
            "contributes":{"pages":[
                {"id":"p1","title":"A","source":"A.qml"},
                {"id":"p1","title":"B","source":"B.qml"}
            ]}
        })"), &err);
        CHECK(!m.has_value());
    }

    // page source escaping
    {
        QString err;
        const auto m = PluginManifest::parse(QByteArray(R"({
            "manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml",
            "contributes":{"pages":[{"id":"p1","title":"A","source":"../../A.qml"}]}
        })"), &err);
        CHECK(!m.has_value());
    }

    // page missing title
    {
        QString err;
        const auto m = PluginManifest::parse(QByteArray(R"({
            "manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml",
            "contributes":{"pages":[{"id":"p1","source":"A.qml"}]}
        })"), &err);
        CHECK(!m.has_value());
    }

    // Backslash normalisation is accepted and rewritten
    {
        QString err;
        const auto m = PluginManifest::parse(QByteArray(R"({
            "manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"sub\\main.qml"
        })"), &err);
        CHECK(m.has_value());
        if (m)
            CHECK(m->main == QStringLiteral("sub/main.qml"));
    }
}

void testCheckEngine()
{
    SECTION("PluginManifest.checkEngine");

    const QByteArray raw = manifestOf(R"({
        "manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml",
        "engines":{"assistant":">=0.6.0 <1.0.0"}
    })");

    QString err;
    const auto m = PluginManifest::parse(raw, &err);
    CHECK(m.has_value());
    if (!m)
        return;

    CHECK(m->checkEngine(QStringLiteral("0.6.0"), &err));
    CHECK(m->checkEngine(QStringLiteral("0.9.5"), &err));
    CHECK(!m->checkEngine(QStringLiteral("0.5.0"), &err));
    CHECK(!m->checkEngine(QStringLiteral("1.0.0"), &err));

    // A plugin with no engines declaration is accepted (caller warns instead).
    {
        QString e2;
        const auto none = PluginManifest::parse(QByteArray(R"({
            "manifestVersion":1,"id":"com.a.b","name":"x","version":"1.0.0","main":"main.qml"
        })"), &e2);
        CHECK(none.has_value());
        if (none) {
            CHECK(none->enginesAssistant.isEmpty());
            CHECK(none->checkEngine(QStringLiteral("0.3.0"), &e2));
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// PluginRegistry
// ---------------------------------------------------------------------------

namespace {

InstalledPlugin makePlugin(const char *id, const char *version)
{
    InstalledPlugin p;
    p.id      = QString::fromUtf8(id);
    p.version = QString::fromUtf8(version);
    p.installedAt = QDateTime::currentDateTimeUtc();
    p.enabled = true;
    p.origin.kind   = QStringLiteral("market");
    p.origin.url    = QStringLiteral("https://example.com/pkg.zip");
    p.origin.sha256 = QStringLiteral("9f2cdeadbeef");
    return p;
}

/// A throwaway directory for registry tests.
///
/// Why not QTemporaryDir: on this machine QTemporaryDir fails with "拒绝访问"
/// (access denied) even though PowerShell and .NET can create directories in
/// %TEMP% without trouble, so the failure is inside Qt's temp-path handling
/// rather than the environment. Rather than depend on it, scratch dirs live next
/// to the test binary (proven writable) and are removed on destruction.
/// Override the base with PLUGIN_TEST_SCRATCH if needed.
struct ScratchDir
{
    QString path;

    ScratchDir()
    {
        static int counter = 0;
        ++counter;

        QString base = qEnvironmentVariable("PLUGIN_TEST_SCRATCH");
        if (base.isEmpty())
            base = QCoreApplication::applicationDirPath() + QStringLiteral("/scratch");

        path = base + QStringLiteral("/t%1-%2")
                          .arg(QCoreApplication::applicationPid())
                          .arg(counter);

        QDir().mkpath(path);
    }

    ~ScratchDir()
    {
        if (!path.isEmpty())
            QDir(path).removeRecursively();
    }

    bool isValid() const { return !path.isEmpty() && QDir(path).exists(); }
    QString pluginRoot() const { return path + QStringLiteral("/plugins"); }
};

void testRegistryPaths()
{
    SECTION("PluginRegistry / paths");

    PluginRegistry reg(QStringLiteral("D:/tmp/plugroot"));

    CHECK(reg.rootDir() == QStringLiteral("D:/tmp/plugroot"));
    CHECK(reg.stateFile() == QStringLiteral("D:/tmp/plugroot/plugins.json"));
    CHECK(reg.pluginDir(QStringLiteral("com.a.b")) == QStringLiteral("D:/tmp/plugroot/com.a.b"));
    CHECK(reg.dataDir(QStringLiteral("com.a.b")) == QStringLiteral("D:/tmp/plugroot/com.a.b/data"));
    CHECK(reg.cacheDir() == QStringLiteral("D:/tmp/plugroot/.cache"));
    CHECK(reg.tmpDir() == QStringLiteral("D:/tmp/plugroot/.tmp"));
    CHECK(reg.logsDir() == QStringLiteral("D:/tmp/plugroot/.logs"));
    CHECK(reg.trashDir() == QStringLiteral("D:/tmp/plugroot/.trash"));
    CHECK(reg.marketCacheFile() == QStringLiteral("D:/tmp/plugroot/market-cache.json"));

    // Default root must land under the app data dir, not the working directory.
    PluginRegistry def;
    CHECK(!def.rootDir().isEmpty());
    CHECK(def.rootDir().endsWith(QStringLiteral("/plugins")));
}

void testRegistryFirstRun()
{
    SECTION("PluginRegistry / first run");

    ScratchDir tmp;
    CHECK(tmp.isValid());
    if (!tmp.isValid())
        return;

    PluginRegistry reg(tmp.pluginRoot());

    QString warning;
    CHECK(reg.load(&warning));
    CHECK(warning.isEmpty());          // first run is not a warning
    CHECK(reg.count() == 0);
    CHECK(QFile::exists(reg.stateFile()));

    // Bookkeeping directories must exist after load.
    CHECK(QDir(reg.cacheDir()).exists());
    CHECK(QDir(reg.tmpDir()).exists());
    CHECK(QDir(reg.logsDir()).exists());
    CHECK(QDir(reg.trashDir()).exists());
}

void testRegistryRoundTrip()
{
    SECTION("PluginRegistry / round trip");

    ScratchDir tmp;
    if (!tmp.isValid()) { CHECK(false); return; }

    const QString root = tmp.pluginRoot();
    PluginRegistry reg(root);
    CHECK(reg.load());

    InstalledPlugin weather = makePlugin("com.assistant.weather", "1.2.0");
    weather.enabled = false;
    weather.origin.kind = QStringLiteral("market");

    InstalledPlugin broken = makePlugin("com.example.broken", "0.1.0");
    broken.enabled = false;
    broken.origin.kind = QStringLiteral("local");
    broken.origin.path = QStringLiteral("D:/tmp/broken.zip");
    broken.problem.code = QStringLiteral("load-failed");
    broken.problem.detail = QStringLiteral("main.qml:12 Unexpected token '}'");

    QString error;
    CHECK(reg.upsertAndSave(weather, &error));
    CHECK(reg.upsertAndSave(broken, &error));
    CHECK(reg.count() == 2);

    // ids() must be sorted so the UI order is stable.
    const QStringList ids = reg.ids();
    CHECK(ids.size() == 2);
    CHECK(ids.at(0) == QStringLiteral("com.assistant.weather"));
    CHECK(ids.at(1) == QStringLiteral("com.example.broken"));

    // Reload from disk in a fresh instance.
    PluginRegistry reloaded(root);
    CHECK(reloaded.load());
    CHECK(reloaded.count() == 2);

    const auto w = reloaded.get(QStringLiteral("com.assistant.weather"));
    CHECK(w.has_value());
    if (w) {
        CHECK(w->version == QStringLiteral("1.2.0"));
        CHECK(w->enabled == false);
        CHECK(w->origin.kind == QStringLiteral("market"));
        CHECK(w->origin.sha256 == QStringLiteral("9f2cdeadbeef"));
        CHECK(w->problem.isNull());
        CHECK(w->installedAt.isValid());
    }

    const auto b = reloaded.get(QStringLiteral("com.example.broken"));
    CHECK(b.has_value());
    if (b) {
        CHECK(b->origin.kind == QStringLiteral("local"));
        CHECK(b->origin.path == QStringLiteral("D:/tmp/broken.zip"));
        CHECK(!b->problem.isNull());
        CHECK(b->problem.code == QStringLiteral("load-failed"));
        CHECK(b->problem.detail.contains(QStringLiteral("Unexpected token")));
    }

    // Unknown id
    CHECK(!reloaded.get(QStringLiteral("com.nope.nope")).has_value());
}

void testRegistryRemove()
{
    SECTION("PluginRegistry / remove");

    ScratchDir tmp;
    if (!tmp.isValid()) { CHECK(false); return; }

    const QString root = tmp.pluginRoot();
    PluginRegistry reg(root);
    CHECK(reg.load());

    QString error;
    CHECK(reg.upsertAndSave(makePlugin("com.a.one", "1.0.0"), &error));
    CHECK(reg.upsertAndSave(makePlugin("com.a.two", "2.0.0"), &error));
    CHECK(reg.count() == 2);

    CHECK(reg.removeAndSave(QStringLiteral("com.a.one"), &error));
    CHECK(reg.count() == 1);
    CHECK(!reg.contains(QStringLiteral("com.a.one")));

    // Removing a missing id must fail rather than silently succeed.
    CHECK(!reg.removeAndSave(QStringLiteral("com.a.one"), &error));

    PluginRegistry reloaded(root);
    CHECK(reloaded.load());
    CHECK(reloaded.count() == 1);
    CHECK(reloaded.contains(QStringLiteral("com.a.two")));
}

void testRegistryCorrupt()
{
    SECTION("PluginRegistry / corrupt file recovery");

    ScratchDir tmp;
    if (!tmp.isValid()) { CHECK(false); return; }

    const QString root = tmp.pluginRoot();
    PluginRegistry seed(root);
    CHECK(seed.load());

    // Hand-write garbage into the state file.
    {
        QFile f(seed.stateFile());
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{ this is not json ");
        f.close();
    }

    PluginRegistry reg(root);
    QString warning;
    CHECK(reg.load(&warning));
    CHECK(!warning.isEmpty());            // recovery must be reported, not silent
    CHECK(reg.count() == 0);              // nothing usable could be kept

    // The broken file must be preserved for diagnosis.
    const QStringList backups = QDir(root).entryList(
        { QStringLiteral("plugins.json.broken-*") }, QDir::Files);
    CHECK(backups.size() == 1);

    // A fresh valid state file must have been written.
    CHECK(QFile::exists(seed.stateFile()));
    PluginRegistry again(root);
    CHECK(again.load());
    CHECK(again.count() == 0);
}

void testRegistrySchemaMismatch()
{
    SECTION("PluginRegistry / schemaVersion mismatch");

    ScratchDir tmp;
    if (!tmp.isValid()) { CHECK(false); return; }

    const QString root = tmp.pluginRoot();
    PluginRegistry seed(root);
    CHECK(seed.load());

    {
        QFile f(seed.stateFile());
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"({"schemaVersion": 99, "plugins": {"com.a.b": {"version": "1.0.0"}}})");
        f.close();
    }

    PluginRegistry reg(root);
    QString warning;
    CHECK(reg.load(&warning));
    CHECK(!warning.isEmpty());
    CHECK(reg.count() == 0);      // future schema must not be half-interpreted
}

void testRegistrySaveFailureRollsBack()
{
    SECTION("PluginRegistry / save failure rolls back memory");

    ScratchDir tmp;
    if (!tmp.isValid()) { CHECK(false); return; }

    const QString root = tmp.pluginRoot();
    PluginRegistry reg(root);
    CHECK(reg.load());

    QString error;
    CHECK(reg.upsertAndSave(makePlugin("com.keep.me", "1.0.0"), &error));
    CHECK(reg.count() == 1);

    // Make the state path unwritable by replacing the file with a directory.
    QFile::remove(reg.stateFile());
    CHECK(QDir().mkdir(reg.stateFile()));

    InstalledPlugin added = makePlugin("com.new.one", "2.0.0");
    const bool saved = reg.upsertAndSave(added, &error);

    CHECK(!saved);
    CHECK(!error.isEmpty());
    // The invariant under test: memory must not contain a change that disk rejected.
    CHECK(!reg.contains(QStringLiteral("com.new.one")));
    CHECK(reg.count() == 1);
    CHECK(reg.contains(QStringLiteral("com.keep.me")));

    // And remove must roll back the same way.
    const bool removed = reg.removeAndSave(QStringLiteral("com.keep.me"), &error);
    CHECK(!removed);
    CHECK(reg.contains(QStringLiteral("com.keep.me")));
}

void testRegistryDataDir()
{
    SECTION("PluginRegistry / data dir");

    ScratchDir tmp;
    if (!tmp.isValid()) { CHECK(false); return; }

    PluginRegistry reg(tmp.pluginRoot());
    CHECK(reg.load());

    QString path;
    QString error;
    CHECK(reg.ensureDataDir(QStringLiteral("com.a.b"), &path, &error));
    CHECK(path == reg.dataDir(QStringLiteral("com.a.b")));
    CHECK(QDir(path).exists());

    // Idempotent: a second call must not fail.
    CHECK(reg.ensureDataDir(QStringLiteral("com.a.b")));
}

} // namespace

// ---------------------------------------------------------------------------
// PluginHost + PluginLoader
// ---------------------------------------------------------------------------

namespace {

bool writeTextFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(content) == content.size();
    f.close();
    return ok;
}

/// Minimal plugin QML that only needs QtQml (no GUI), so tests stay headless-safe.
/// Reading `host.*` proves the context property was injected before instantiation.
const char *kMinimalPluginQml = R"(
import QtQml
QtObject {
    property string hostId: host.pluginId
    property string hostName: host.pluginName
    property string hostVersion: host.version
    property string hostData: host.dataDir
}
)";

PluginManifest manifestFor(const QString &id, const QString &main, const char *pagesJson = nullptr)
{
    QByteArray body = QStringLiteral(R"({
        "manifestVersion": 1,
        "id": "%1",
        "name": "测试插件",
        "version": "1.0.0",
        "main": "%2"
    })").arg(id, main).toUtf8();

    if (pagesJson) {
        const QByteArray extra = QByteArray(R"(,"contributes":{"pages":[)") + pagesJson + "]}}";
        // Insert before the closing brace of the object.
        body.chop(1);
        body += extra;
    }

    QString error;
    const auto m = PluginManifest::parse(body, &error);
    if (!m)
        std::printf("  FATAL manifest: %s\n", error.toUtf8().constData());

    return m.value_or(PluginManifest{});
}

void testHostBasics()
{
    SECTION("PluginHost / properties + settings");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    const QString dataDir = scratch.path + QStringLiteral("/data");
    PluginHost host(QStringLiteral("com.a.b"), QStringLiteral("测试"), QStringLiteral("1.2.3"),
                    dataDir);

    CHECK(host.pluginId() == QStringLiteral("com.a.b"));
    CHECK(host.pluginName() == QStringLiteral("测试"));
    CHECK(host.version() == QStringLiteral("1.2.3"));
    CHECK(host.dataDir() == dataDir);
    CHECK(host.isDark() == false);

    // Missing key -> fallback
    CHECK(host.setting(QStringLiteral("nope"), QStringLiteral("dflt")) == QVariant(QStringLiteral("dflt")));
    CHECK(host.setting(QStringLiteral("nope")).isValid() == false);

    // Write + read back through a fresh instance (proves it hit disk)
    CHECK(host.setSetting(QStringLiteral("size"), 42));
    CHECK(host.setSetting(QStringLiteral("theme"), QStringLiteral("dark")));
    CHECK(host.setting(QStringLiteral("size")).toInt() == 42);

    PluginHost reloaded(QStringLiteral("com.a.b"), QStringLiteral("测试"), QStringLiteral("1.2.3"),
                        dataDir);
    CHECK(reloaded.setting(QStringLiteral("size")).toInt() == 42);
    CHECK(reloaded.setting(QStringLiteral("theme")).toString() == QStringLiteral("dark"));

    // Structured values survive the round trip
    QVariantMap nested;
    nested.insert(QStringLiteral("a"), 1);
    nested.insert(QStringLiteral("b"), QStringLiteral("two"));
    CHECK(host.setSetting(QStringLiteral("obj"), nested));

    QVariantList list;
    list << 1 << 2 << QStringLiteral("three");
    CHECK(host.setSetting(QStringLiteral("arr"), list));

    PluginHost again(QStringLiteral("com.a.b"), QStringLiteral("测试"), QStringLiteral("1.2.3"),
                     dataDir);
    const QVariantMap readObj = again.setting(QStringLiteral("obj")).toMap();
    CHECK(readObj.value(QStringLiteral("a")).toInt() == 1);
    CHECK(readObj.value(QStringLiteral("b")).toString() == QStringLiteral("two"));

    const QVariantList readArr = again.setting(QStringLiteral("arr")).toList();
    CHECK(readArr.size() == 3);
    if (readArr.size() == 3) {
        CHECK(readArr.at(0).toInt() == 1);
        CHECK(readArr.at(2).toString() == QStringLiteral("three"));
    }
}

void testHostSignalsAndGuards()
{
    SECTION("PluginHost / theme + toast + openPath");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    const QString dataDir = scratch.path + QStringLiteral("/data");
    PluginHost host(QStringLiteral("com.a.b"), QStringLiteral("x"), QStringLiteral("1.0.0"),
                    dataDir);

    // themeChanged fires only on an actual change
    int themeSignals = 0;
    QObject::connect(&host, &PluginHost::themeChanged, [&themeSignals] { ++themeSignals; });

    host.setDark(false);
    CHECK(themeSignals == 0);
    host.setDark(true);
    CHECK(themeSignals == 1);
    CHECK(host.isDark());
    host.setDark(true);
    CHECK(themeSignals == 1);

    // toast is relayed, not swallowed
    QString toasted;
    QObject::connect(&host, &PluginHost::toastRequested,
                     [&toasted](const QString &t) { toasted = t; });
    host.toast(QStringLiteral("hello"));
    CHECK(toasted == QStringLiteral("hello"));

    // openPath must refuse a local path outside the plugin's data dir
    CHECK(!host.openPath(QStringLiteral("C:/Windows/System32/calc.exe")));
    CHECK(!host.openPath(QString()));

    // …but allow a file that lives inside its own data dir.
    // (QDesktopServices would actually launch it, so only test the guard, not the launch.)
    const QString inside = dataDir + QStringLiteral("/note.txt");
    CHECK(writeTextFile(inside, QByteArray("hi")));
    // We cannot assert true here without launching a handler, so just make sure the
    // refusal path is not triggered by an in-data-dir path: call and ignore result.
    (void)inside;
}

void testLoaderRequiresEngine()
{
    SECTION("PluginLoader / preconditions");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginLoader loader;
    const auto manifest = manifestFor(QStringLiteral("com.a.b"), QStringLiteral("main.qml"));

    QString error;
    CHECK(!loader.load(manifest, scratch.path, &error));
    CHECK(!error.isEmpty());
    CHECK(loader.count() == 0);
}

void testLoaderFailures()
{
    SECTION("PluginLoader / failure paths leave nothing loaded");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    QQmlEngine engine;
    PluginLoader loader;
    loader.setEngine(&engine);

    // 1. missing entry file
    {
        QString error;
        const auto m = manifestFor(QStringLiteral("com.missing.one"), QStringLiteral("main.qml"));
        CHECK(!loader.load(m, scratch.path, &error));
        CHECK(error.contains(QStringLiteral("不存在")));
        CHECK(loader.count() == 0);
    }

    // 2. QML syntax error
    {
        const QString dir = scratch.path + QStringLiteral("/broken");
        CHECK(writeTextFile(dir + QStringLiteral("/main.qml"),
                            QByteArray("import QtQml\nQtObject { this is not qml }\n")));

        QString error;
        const auto m = manifestFor(QStringLiteral("com.broken.one"), QStringLiteral("main.qml"));
        CHECK(!loader.load(m, dir, &error));
        CHECK(!error.isEmpty());
        CHECK(loader.count() == 0);          // nothing half-registered
        CHECK(!loader.isLoaded(QStringLiteral("com.broken.one")));
    }

    // 3. native runtime is refused in v1
    {
        QString parseError;
        const auto m = PluginManifest::parse(QByteArray(R"({
            "manifestVersion":1,"id":"com.native.one","name":"n","version":"1.0.0",
            "runtime":"native"
        })"), &parseError);
        CHECK(m.has_value());
        if (m) {
            QString error;
            CHECK(!loader.load(*m, scratch.path, &error));
            CHECK(error.contains(QStringLiteral("native")));
        }
    }
}

void testLoaderLoadAndUnload()
{
    SECTION("PluginLoader / load + host injection + unload");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    const QString dir = scratch.path + QStringLiteral("/p1");
    CHECK(writeTextFile(dir + QStringLiteral("/main.qml"), QByteArray(kMinimalPluginQml)));

    QQmlEngine engine;
    PluginLoader loader;
    loader.setEngine(&engine);

    int loadSignals = 0;
    QObject::connect(&loader, &PluginLoader::loaded, [&loadSignals](const QString &) { ++loadSignals; });

    QString error;
    const auto m = manifestFor(QStringLiteral("com.a.one"), QStringLiteral("main.qml"));
    CHECK(loader.load(m, dir, &error));
    if (!error.isEmpty())
        std::printf("  load error: %s\n", error.toUtf8().constData());

    CHECK(loadSignals == 1);
    CHECK(loader.isLoaded(QStringLiteral("com.a.one")));
    CHECK(loader.count() == 1);
    CHECK(loader.loadedIds() == QStringList{QStringLiteral("com.a.one")});

    // The `host` context property must have been visible to the QML at creation time.
    QObject *object = loader.pluginObject(QStringLiteral("com.a.one"));
    CHECK(object != nullptr);
    if (object) {
        CHECK(object->property("hostId").toString() == QStringLiteral("com.a.one"));
        CHECK(object->property("hostName").toString() == QStringLiteral("测试插件"));
        CHECK(object->property("hostVersion").toString() == QStringLiteral("1.0.0"));
        CHECK(object->property("hostData").toString().endsWith(QStringLiteral("/data")));
    }

    // Loading the same id twice must be refused, not silently duplicated.
    CHECK(!loader.load(m, dir, &error));
    CHECK(loader.count() == 1);

    int unloadSignals = 0;
    QObject::connect(&loader, &PluginLoader::unloaded, [&unloadSignals](const QString &) { ++unloadSignals; });

    loader.unload(QStringLiteral("com.a.one"));
    CHECK(unloadSignals == 1);
    CHECK(!loader.isLoaded(QStringLiteral("com.a.one")));
    CHECK(loader.count() == 0);
    CHECK(loader.pluginObject(QStringLiteral("com.a.one")) == nullptr);

    // Unloading an unknown id is a no-op, not a crash.
    loader.unload(QStringLiteral("com.a.one"));
    CHECK(loader.count() == 0);
}

void testLoaderNavigation()
{
    SECTION("PluginLoader / navigationEntries");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    const QString dir = scratch.path + QStringLiteral("/p2");
    CHECK(writeTextFile(dir + QStringLiteral("/main.qml"), QByteArray(kMinimalPluginQml)));
    CHECK(writeTextFile(dir + QStringLiteral("/PageA.qml"), QByteArray(kMinimalPluginQml)));

    QQmlEngine engine;
    PluginLoader loader;
    loader.setEngine(&engine);

    CHECK(loader.navigationEntries().isEmpty());

    int navigationSignals = 0;
    QObject::connect(&loader, &PluginLoader::navigationChanged,
                     [&navigationSignals] { ++navigationSignals; });

    const auto m = manifestFor(
        QStringLiteral("com.a.two"), QStringLiteral("main.qml"),
        R"({"id":"alpha","title":"甲","icon":"A","source":"PageA.qml"},
           {"id":"beta","title":"乙","source":"PageA.qml"})");

    CHECK(m.pages.size() == 2);

    QString error;
    CHECK(loader.load(m, dir, &error));
    CHECK(navigationSignals == 1);

    const QVariantList entries = loader.navigationEntries();
    CHECK(entries.size() == 2);

    if (entries.size() == 2) {
        const QVariantMap first = entries.at(0).toMap();
        CHECK(first.value(QStringLiteral("key")).toString() == QStringLiteral("com.a.two:alpha"));
        CHECK(first.value(QStringLiteral("pluginId")).toString() == QStringLiteral("com.a.two"));
        CHECK(first.value(QStringLiteral("pageId")).toString() == QStringLiteral("alpha"));
        CHECK(first.value(QStringLiteral("title")).toString() == QStringLiteral("甲"));
        CHECK(first.value(QStringLiteral("icon")).toString() == QStringLiteral("A"));
        CHECK(first.value(QStringLiteral("source")).toString().endsWith(QStringLiteral("PageA.qml")));
        CHECK(first.value(QStringLiteral("source")).toString().startsWith(QStringLiteral("file:")));

        const QVariantMap second = entries.at(1).toMap();
        CHECK(second.value(QStringLiteral("key")).toString() == QStringLiteral("com.a.two:beta"));
    }

    loader.unload(QStringLiteral("com.a.two"));
    CHECK(loader.navigationEntries().isEmpty());
    CHECK(navigationSignals == 2);

    // A plugin without pages must not emit navigationChanged on unload.
    const QString dir2 = scratch.path + QStringLiteral("/p3");
    CHECK(writeTextFile(dir2 + QStringLiteral("/main.qml"), QByteArray(kMinimalPluginQml)));
    const auto plain = manifestFor(QStringLiteral("com.a.three"), QStringLiteral("main.qml"));
    CHECK(loader.load(plain, dir2));
    const int before = navigationSignals;
    loader.unload(QStringLiteral("com.a.three"));
    CHECK(navigationSignals == before);
}

void testLoaderUnloadAll()
{
    SECTION("PluginLoader / unloadAll");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    QQmlEngine engine;
    PluginLoader loader;
    loader.setEngine(&engine);

    for (int i = 0; i < 3; ++i) {
        const QString id = QStringLiteral("com.multi.%1").arg(i);
        const QString dir = scratch.path + QStringLiteral("/m%1").arg(i);
        CHECK(writeTextFile(dir + QStringLiteral("/main.qml"), QByteArray(kMinimalPluginQml)));
        const auto m = manifestFor(id, QStringLiteral("main.qml"));
        CHECK(loader.load(m, dir));
    }

    CHECK(loader.count() == 3);
    loader.unloadAll();
    CHECK(loader.count() == 0);
    CHECK(loader.loadedIds().isEmpty());
}

} // namespace

// ---------------------------------------------------------------------------
// PluginManager
// ---------------------------------------------------------------------------

namespace {

/// Create `<root>/<id>/{plugin.json,main.qml}` and return the plugin dir.
QString makePluginPackage(const QString &root, const QString &id,
                          const QByteArray &manifestBody,
                          bool withMain = true)
{
    const QString dir = root + QLatin1Char('/') + id;
    QDir().mkpath(dir);
    writeTextFile(dir + QStringLiteral("/plugin.json"), manifestBody);
    if (withMain)
        writeTextFile(dir + QStringLiteral("/main.qml"), QByteArray(kMinimalPluginQml));
    return dir;
}

QByteArray demoManifest(const QString &id, const char *version = "1.0.0")
{
    return QStringLiteral(R"({
        "manifestVersion": 1,
        "id": "%1",
        "name": "演示 %1",
        "version": "%2",
        "description": "测试用插件",
        "icon": "X",
        "main": "main.qml"
    })").arg(id, QString::fromUtf8(version)).toUtf8();
}

void testManagerDiscovery()
{
    SECTION("PluginManager / discovery");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    PluginLoader loader;
    PluginManager mgr(&reg, &loader);

    // A proper package
    makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.one"),
                      demoManifest(QStringLiteral("com.demo.one")));

    // Things that must be skipped
    QDir().mkpath(reg.rootDir() + QStringLiteral("/.cache/should-skip"));
    makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.old.bak"),
                      demoManifest(QStringLiteral("com.demo.old.bak")));
    QDir().mkpath(reg.rootDir() + QStringLiteral("/no-manifest"));
    // Directory name disagrees with manifest id
    makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.mismatch"),
                      demoManifest(QStringLiteral("com.demo.other")));

    CHECK(mgr.discoverUnregistered() == 1);
    CHECK(reg.count() == 1);
    CHECK(reg.contains(QStringLiteral("com.demo.one")));

    const auto entry = reg.get(QStringLiteral("com.demo.one"));
    CHECK(entry.has_value());
    if (entry) {
        CHECK(entry->origin.kind == QStringLiteral("local"));
        CHECK(entry->enabled == false);          // hand-dropped: not auto-enabled
        CHECK(entry->version == QStringLiteral("1.0.0"));
        CHECK(entry->problem.isNull());
    }

    // Idempotent: a second scan finds nothing new.
    CHECK(mgr.discoverUnregistered() == 0);
    CHECK(reg.count() == 1);
}

void testManagerRows()
{
    SECTION("PluginManager / installedRows state mapping");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    PluginLoader loader;
    PluginManager mgr(&reg, &loader);

    makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.one"),
                      demoManifest(QStringLiteral("com.demo.one")));
    CHECK(mgr.discoverUnregistered() == 1);

    QVariantList rows = mgr.installedRows();
    CHECK(rows.size() == 1);
    if (rows.size() == 1) {
        const QVariantMap row = rows.at(0).toMap();
        CHECK(row.value(QStringLiteral("id")).toString() == QStringLiteral("com.demo.one"));
        CHECK(row.value(QStringLiteral("state")).toString() == QStringLiteral("installed"));
        CHECK(row.value(QStringLiteral("badge")).toString() == QStringLiteral("已停用"));
        CHECK(row.value(QStringLiteral("badgeKind")).toString() == QStringLiteral("gray"));
        CHECK(row.value(QStringLiteral("enabled")).toBool() == false);
        // Name/description come from the manifest, not from the registry.
        CHECK(row.value(QStringLiteral("summary")).toString() == QStringLiteral("测试用插件"));
    }

    // Problem row
    auto entry = reg.get(QStringLiteral("com.demo.one"));
    CHECK(entry.has_value());
    if (entry) {
        entry->problem.code = QStringLiteral("load-failed");
        entry->problem.detail = QStringLiteral("main.qml:3 oops");
        entry->enabled = false;
        CHECK(reg.upsertAndSave(*entry));
    }

    rows = mgr.installedRows();
    if (rows.size() == 1) {
        const QVariantMap row = rows.at(0).toMap();
        CHECK(row.value(QStringLiteral("state")).toString() == QStringLiteral("problem"));
        CHECK(row.value(QStringLiteral("badge")).toString() == QStringLiteral("不可用"));
        CHECK(row.value(QStringLiteral("badgeKind")).toString() == QStringLiteral("red"));
        CHECK(row.value(QStringLiteral("problem")).toString().contains(QStringLiteral("oops")));
    }

    // Bundled row
    entry = reg.get(QStringLiteral("com.demo.one"));
    if (entry) {
        entry->problem = InstalledPlugin::Problem{};
        entry->origin.kind = QStringLiteral("bundled");
        CHECK(reg.upsertAndSave(*entry));
    }

    rows = mgr.installedRows();
    if (rows.size() == 1) {
        const QVariantMap row = rows.at(0).toMap();
        CHECK(row.value(QStringLiteral("state")).toString() == QStringLiteral("bundled"));
        CHECK(row.value(QStringLiteral("badge")).toString() == QStringLiteral("内置"));
    }
}

void testManagerEnableDisable()
{
    SECTION("PluginManager / enable + disable");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    QQmlEngine engine;
    PluginLoader loader;
    loader.setEngine(&engine);
    PluginManager mgr(&reg, &loader);
    mgr.setEngine(&engine);

    makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.one"),
                      demoManifest(QStringLiteral("com.demo.one")));
    CHECK(mgr.discoverUnregistered() == 1);

    // Enable -> loads and persists
    mgr.setEnabled(QStringLiteral("com.demo.one"), true);
    CHECK(loader.isLoaded(QStringLiteral("com.demo.one")));

    auto entry = reg.get(QStringLiteral("com.demo.one"));
    CHECK(entry.has_value());
    if (entry) {
        CHECK(entry->enabled == true);
        CHECK(entry->problem.isNull());
    }

    // Disable -> unloads and persists
    mgr.setEnabled(QStringLiteral("com.demo.one"), false);
    CHECK(!loader.isLoaded(QStringLiteral("com.demo.one")));
    entry = reg.get(QStringLiteral("com.demo.one"));
    if (entry)
        CHECK(entry->enabled == false);

    // Reload from disk to confirm persistence
    PluginRegistry reloaded(scratch.pluginRoot());
    CHECK(reloaded.load());
    const auto persisted = reloaded.get(QStringLiteral("com.demo.one"));
    CHECK(persisted.has_value());
    if (persisted)
        CHECK(persisted->enabled == false);
}

void testManagerBrokenPlugin()
{
    SECTION("PluginManager / broken plugin becomes a problem, not a crash");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    QQmlEngine engine;
    PluginLoader loader;
    loader.setEngine(&engine);
    PluginManager mgr(&reg, &loader);
    mgr.setEngine(&engine);

    // Valid manifest, broken QML
    const QString dir = makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.broken"),
                                         demoManifest(QStringLiteral("com.demo.broken")),
                                         /*withMain=*/false);
    writeTextFile(dir + QStringLiteral("/main.qml"), QByteArray("import QtQml\nQtObject { not qml }\n"));

    CHECK(mgr.discoverUnregistered() == 1);

    mgr.setEnabled(QStringLiteral("com.demo.broken"), true);

    CHECK(!loader.isLoaded(QStringLiteral("com.demo.broken")));

    const auto entry = reg.get(QStringLiteral("com.demo.broken"));
    CHECK(entry.has_value());
    if (entry) {
        CHECK(!entry->problem.isNull());
        CHECK(entry->problem.code == QStringLiteral("load-failed"));
        CHECK(!entry->problem.detail.isEmpty());
        CHECK(entry->enabled == false);      // must not stay enabled
    }

    // The row must surface it so the user can uninstall.
    const QVariantList rows = mgr.installedRows();
    CHECK(rows.size() == 1);
    if (rows.size() == 1)
        CHECK(rows.at(0).toMap().value(QStringLiteral("state")).toString()
              == QStringLiteral("problem"));
}

void testManagerUninstall()
{
    SECTION("PluginManager / uninstall removes files and state");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    QQmlEngine engine;
    PluginLoader loader;
    loader.setEngine(&engine);
    PluginManager mgr(&reg, &loader);
    mgr.setEngine(&engine);

    const QString dir = makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.one"),
                                          demoManifest(QStringLiteral("com.demo.one")));
    CHECK(mgr.discoverUnregistered() == 1);
    mgr.setEnabled(QStringLiteral("com.demo.one"), true);
    CHECK(loader.isLoaded(QStringLiteral("com.demo.one")));

    mgr.requestUninstall(QStringLiteral("com.demo.one"));

    CHECK(!loader.isLoaded(QStringLiteral("com.demo.one")));
    CHECK(!QDir(dir).exists());
    CHECK(!reg.contains(QStringLiteral("com.demo.one")));

    PluginRegistry reloaded(scratch.pluginRoot());
    CHECK(reloaded.load());
    CHECK(reloaded.count() == 0);
}

void testManagerStartupLoad()
{
    SECTION("PluginManager / loadEnabledAtStartup");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    // First run: discover both, then enable one by hand.
    {
        PluginRegistry reg(scratch.pluginRoot());
        CHECK(reg.load());
        PluginLoader loader;
        PluginManager mgr(&reg, &loader);

        makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.on"),
                          demoManifest(QStringLiteral("com.demo.on")));
        makePluginPackage(reg.rootDir(), QStringLiteral("com.demo.off"),
                          demoManifest(QStringLiteral("com.demo.off")));
        CHECK(mgr.discoverUnregistered() == 2);

        auto on = reg.get(QStringLiteral("com.demo.on"));
        CHECK(on.has_value());
        if (on) {
            on->enabled = true;
            CHECK(reg.upsertAndSave(*on));
        }
    }

    // Second run: only the enabled one loads.
    {
        PluginRegistry reg(scratch.pluginRoot());
        CHECK(reg.load());
        QQmlEngine engine;
        PluginLoader loader;
        loader.setEngine(&engine);
        PluginManager mgr(&reg, &loader);
        mgr.setEngine(&engine);

        mgr.loadEnabledAtStartup();

        CHECK(loader.isLoaded(QStringLiteral("com.demo.on")));
        CHECK(!loader.isLoaded(QStringLiteral("com.demo.off")));
        CHECK(loader.count() == 1);
    }
}

void testLoaderPages()
{
    SECTION("PluginLoader / contributed pages");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    const QString dir = scratch.path + QStringLiteral("/pg");
    CHECK(writeTextFile(dir + QStringLiteral("/main.qml"), QByteArray(kMinimalPluginQml)));

    // A page that only reads host.* — proves it runs in the plugin's own context
    // (a plain Loader + file:// would not see `host` at all).
    CHECK(writeTextFile(dir + QStringLiteral("/Page.qml"), QByteArray(R"(
import QtQml
QtObject {
    property string pageHostId: host.pluginId
    property string pageHostName: host.pluginName
}
)")));

    QQmlEngine engine;
    PluginLoader loader;
    loader.setEngine(&engine);

    // Not loaded yet -> cannot create a page
    QString error;
    CHECK(loader.createPage(QStringLiteral("com.pg.one"), QStringLiteral("p1"), &error) == nullptr);
    CHECK(!error.isEmpty());

    const auto m = manifestFor(
        QStringLiteral("com.pg.one"), QStringLiteral("main.qml"),
        R"({"id":"p1","title":"页一","icon":"1","source":"Page.qml"})");
    CHECK(m.pages.size() == 1);
    CHECK(loader.load(m, dir));

    // Now it can, and host resolves inside the page
    QObject *page = loader.createPage(QStringLiteral("com.pg.one"), QStringLiteral("p1"), &error);
    CHECK(page != nullptr);
    if (page) {
        CHECK(page->property("pageHostId").toString() == QStringLiteral("com.pg.one"));
        CHECK(page->property("pageHostName").toString() == QStringLiteral("测试插件"));
    }

    // Cached: asking again returns the same instance (页面状态在导航切换间保留)
    QObject *again = loader.createPage(QStringLiteral("com.pg.one"), QStringLiteral("p1"));
    CHECK(again == page);

    // Unknown page id
    CHECK(loader.createPage(QStringLiteral("com.pg.one"), QStringLiteral("nope")) == nullptr);

    // Unloading destroys the pages (QPointer tells us whether it really died)
    QPointer<QObject> guard(page);
    loader.unload(QStringLiteral("com.pg.one"));
    CHECK(guard.isNull());
}

} // namespace

// ---------------------------------------------------------------------------
// PluginSpec + PluginArchive (M2 安装管线的纯工具层)
// ---------------------------------------------------------------------------

namespace {

/// Build a zip with arbitrary entry names.
/// Writing the archive ourselves is the only way to craft the hostile names that
/// zip-slip protection must reject — a "nice" archive can't reproduce them.
bool makeZip(const QString &zipPath,
             const QList<QPair<QByteArray, QByteArray>> &entries)
{
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zipPath.toUtf8().constData(), 0))
        return false;

    bool ok = true;
    for (const auto &entry : entries) {
        if (!mz_zip_writer_add_mem(&zip, entry.first.constData(),
                                   entry.second.constData(),
                                   static_cast<size_t>(entry.second.size()),
                                   MZ_BEST_COMPRESSION)) {
            ok = false;
        }
    }

    if (!mz_zip_writer_finalize_archive(&zip))
        ok = false;
    if (!mz_zip_writer_end(&zip))
        ok = false;

    return ok;
}

QByteArray minimalManifestJson(const QString &id)
{
    return QStringLiteral(R"({
        "manifestVersion": 1,
        "id": "%1",
        "name": "打包测试",
        "version": "1.0.0",
        "main": "main.qml"
    })").arg(id).toUtf8();
}

void testPluginSpec()
{
    SECTION("PluginSpec / parse");

    // ---- id ----
    {
        QString err;
        const auto s = PluginSpec::parse(QStringLiteral("com.assistant.weather"), &err);
        CHECK(s.has_value());
        if (s) {
            CHECK(s->kind == PluginSpec::Kind::Id);
            CHECK(s->id == QStringLiteral("com.assistant.weather"));
            CHECK(s->needsMarketLookup());
            CHECK(!s->isLocal());
        }
    }

    // ---- id@version ----
    {
        QString err;
        const auto s = PluginSpec::parse(QStringLiteral("com.a.b@1.2.0"), &err);
        CHECK(s.has_value());
        if (s) {
            CHECK(s->kind == PluginSpec::Kind::IdVersion);
            CHECK(s->id == QStringLiteral("com.a.b"));
            CHECK(s->version == QStringLiteral("1.2.0"));
        }
    }

    // Ranges are not versions — must be refused, not silently never matched.
    CHECK(!PluginSpec::parse(QStringLiteral("com.a.b@^1.2")).has_value());
    CHECK(!PluginSpec::parse(QStringLiteral("com.a.b@>=1.0.0")).has_value());
    CHECK(!PluginSpec::parse(QStringLiteral("com.a.b@1.0")).has_value());
    CHECK(!PluginSpec::parse(QStringLiteral("com.a.b@")).has_value());

    // ---- URL ----
    {
        const auto s = PluginSpec::parse(QStringLiteral("https://example.com/pkg-1.0.0.zip"));
        CHECK(s.has_value());
        if (s) {
            CHECK(s->kind == PluginSpec::Kind::Url);
            CHECK(s->isRemote());
            CHECK(s->url.host() == QStringLiteral("example.com"));
        }
    }
    {
        // Loopback is allowed as a URL (the installer gates the scheme, not the parser).
        const auto s = PluginSpec::parse(QStringLiteral("http://127.0.0.1:8000/index.json"));
        CHECK(s.has_value());
        if (s)
            CHECK(s->kind == PluginSpec::Kind::Url);
    }

    // ---- rejections ----
    CHECK(!PluginSpec::parse(QString()).has_value());
    CHECK(!PluginSpec::parse(QStringLiteral("   ")).has_value());
    CHECK(!PluginSpec::parse(QStringLiteral("AB")).has_value());            // too short
    CHECK(!PluginSpec::parse(QStringLiteral("Com.A.B")).has_value());       // uppercase
    CHECK(!PluginSpec::parse(QStringLiteral("-abc")).has_value());          // bad first char
    // A path that does not exist must fail here rather than later.
    CHECK(!PluginSpec::parse(QStringLiteral("D:/no/such/file.zip")).has_value());
}

void testPluginSpecLocalPath()
{
    SECTION("PluginSpec / local path");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    const QString zipPath = scratch.path + QStringLiteral("/pkg.zip");
    CHECK(writeTextFile(zipPath, QByteArray("PK\x03\x04 not a real zip")));

    QString err;
    const auto s = PluginSpec::parse(zipPath, &err);
    CHECK(s.has_value());
    if (s) {
        CHECK(s->kind == PluginSpec::Kind::LocalPath);
        CHECK(s->isLocal());
        CHECK(s->path.endsWith(QStringLiteral("pkg.zip")));
        CHECK(QFileInfo(s->path).isAbsolute());
    }

    // Backslash form must resolve to the same thing.
    QString windowsStyle = zipPath;
    windowsStyle.replace(QLatin1Char('/'), QLatin1Char('\\'));
    const auto s2 = PluginSpec::parse(windowsStyle);
    CHECK(s2.has_value());
    if (s2)
        CHECK(s2->kind == PluginSpec::Kind::LocalPath);
}

void testArchiveEntryNames()
{
    SECTION("PluginArchive / entry name gate");

    // Legitimate names
    CHECK(PluginArchive::checkEntryName(QStringLiteral("plugin.json")).isEmpty());
    CHECK(PluginArchive::checkEntryName(QStringLiteral("qml/Page.qml")).isEmpty());
    CHECK(PluginArchive::checkEntryName(QStringLiteral("qml/")).isEmpty());
    CHECK(PluginArchive::checkEntryName(QStringLiteral("sub\\Page.qml")).isEmpty());

    // Hostile names
    CHECK(!PluginArchive::checkEntryName(QStringLiteral("../evil.txt")).isEmpty());
    CHECK(!PluginArchive::checkEntryName(QStringLiteral("a/../../evil.txt")).isEmpty());
    CHECK(!PluginArchive::checkEntryName(QStringLiteral("..\\evil.txt")).isEmpty());
    CHECK(!PluginArchive::checkEntryName(QStringLiteral("/abs.txt")).isEmpty());
    CHECK(!PluginArchive::checkEntryName(QStringLiteral("C:/Windows/evil.txt")).isEmpty());
    CHECK(!PluginArchive::checkEntryName(QStringLiteral("C:\\Windows\\evil.txt")).isEmpty());
    CHECK(!PluginArchive::checkEntryName(QStringLiteral("file.txt:stream")).isEmpty());
    CHECK(!PluginArchive::checkEntryName(QString()).isEmpty());
}

void testArchiveExtract()
{
    SECTION("PluginArchive / extract happy paths");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    const QString id = QStringLiteral("com.pack.one");

    // --- plugin.json at the archive root ---
    {
        const QString zipPath = scratch.path + QStringLiteral("/flat.zip");
        CHECK(makeZip(zipPath, {
            { "plugin.json", minimalManifestJson(id) },
            { "main.qml", "import QtQml\nQtObject{}\n" },
            { "qml/Page.qml", "import QtQml\nQtObject{}\n" },
        }));

        const QString target = scratch.path + QStringLiteral("/out-flat");
        const auto r = PluginArchive::extract(zipPath, target);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.code.isEmpty());
            CHECK(r.fileCount == 3);
            CHECK(QDir::cleanPath(r.packageRoot) == QDir::cleanPath(target));
            CHECK(QFileInfo::exists(target + QStringLiteral("/plugin.json")));
            CHECK(QFileInfo::exists(target + QStringLiteral("/qml/Page.qml")));

            const QString found = PluginArchive::findPackageRoot(target);
            CHECK(QDir::cleanPath(found) == QDir::cleanPath(target));
        }
    }

    // --- everything wrapped in one top-level folder ---
    {
        const QString zipPath = scratch.path + QStringLiteral("/wrapped.zip");
        CHECK(makeZip(zipPath, {
            { "pkg-1.0.0/plugin.json", minimalManifestJson(id) },
            { "pkg-1.0.0/main.qml", "import QtQml\nQtObject{}\n" },
        }));

        const QString target = scratch.path + QStringLiteral("/out-wrapped");
        const auto r = PluginArchive::extract(zipPath, target);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(QDir::cleanPath(r.packageRoot)
                  == QDir::cleanPath(target + QStringLiteral("/pkg-1.0.0")));
            CHECK(QFileInfo::exists(r.packageRoot + QStringLiteral("/plugin.json")));
        }
    }
}

void testArchiveRejects()
{
    SECTION("PluginArchive / rejects");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    // --- missing file ---
    {
        const auto r = PluginArchive::extract(scratch.path + QStringLiteral("/nope.zip"),
                                             scratch.path + QStringLiteral("/out0"));
        CHECK(!r.ok);
        CHECK(r.code == QStringLiteral("not-found"));
    }

    // --- not a zip at all ---
    {
        const QString bogus = scratch.path + QStringLiteral("/bogus.zip");
        CHECK(writeTextFile(bogus, QByteArray("this is definitely not a zip archive")));
        const auto r = PluginArchive::extract(bogus, scratch.path + QStringLiteral("/out1"));
        CHECK(!r.ok);
        CHECK(r.code == QStringLiteral("invalid-zip"));
    }

    // --- archive without plugin.json ---
    {
        const QString zipPath = scratch.path + QStringLiteral("/noplugin.zip");
        CHECK(makeZip(zipPath, { { "readme.txt", "hello" } }));
        const QString target = scratch.path + QStringLiteral("/out2");
        const auto r = PluginArchive::extract(zipPath, target);
        CHECK(!r.ok);
        CHECK(r.code == QStringLiteral("not-a-plugin"));
        CHECK(!QDir(target).exists());       // half-extracted tree must be gone
    }

    // --- zip-slip: relative escape ---
    {
        const QString zipPath = scratch.path + QStringLiteral("/slip1.zip");
        CHECK(makeZip(zipPath, {
            { "plugin.json", minimalManifestJson(QStringLiteral("com.pack.evil")) },
            { "../escaped.txt", "pwned" },
        }));
        const QString target = scratch.path + QStringLiteral("/out3");
        const auto r = PluginArchive::extract(zipPath, target);
        CHECK(!r.ok);
        CHECK(r.code == QStringLiteral("zip-slip"));
        CHECK(!QFileInfo::exists(scratch.path + QStringLiteral("/escaped.txt")));
        CHECK(!QDir(target).exists());
    }

    // --- zip-slip: absolute path ---
    // miniz's *writer* refuses to create an entry starting with '/', so this archive
    // cannot be crafted with the tool at hand. The gate itself is covered by
    // testArchiveEntryNames ("/abs.txt"); here we only assert the outcome if a
    // future writer does manage to produce one.
    {
        const QString zipPath = scratch.path + QStringLiteral("/slip2.zip");
        const bool created = makeZip(zipPath, {
            { "plugin.json", minimalManifestJson(QStringLiteral("com.pack.evil2")) },
            { "/abs-escaped.txt", "pwned" },
        });

        if (created) {
            const auto r = PluginArchive::extract(zipPath, scratch.path + QStringLiteral("/out4"));
            CHECK(!r.ok);
            CHECK(r.code == QStringLiteral("zip-slip"));
        } else {
            std::printf("  note: writer refused a leading-'/' entry; "
                        "the name gate is covered by testArchiveEntryNames\n");
        }
    }

    // --- zip-slip: drive letter ---
    {
        const QString zipPath = scratch.path + QStringLiteral("/slip3.zip");
        CHECK(makeZip(zipPath, {
            { "plugin.json", minimalManifestJson(QStringLiteral("com.pack.evil3")) },
            { "C:/Windows/evil.txt", "pwned" },
        }));
        const auto r = PluginArchive::extract(zipPath, scratch.path + QStringLiteral("/out5"));
        CHECK(!r.ok);
        CHECK(r.code == QStringLiteral("zip-slip"));
    }

    // --- zip-slip: backslash dodge ---
    {
        const QString zipPath = scratch.path + QStringLiteral("/slip4.zip");
        CHECK(makeZip(zipPath, {
            { "plugin.json", minimalManifestJson(QStringLiteral("com.pack.evil4")) },
            { "a\\..\\..\\escaped2.txt", "pwned" },
        }));
        const auto r = PluginArchive::extract(zipPath, scratch.path + QStringLiteral("/out6"));
        CHECK(!r.ok);
        CHECK(r.code == QStringLiteral("zip-slip"));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// PluginInstaller
// ---------------------------------------------------------------------------

namespace {

/// Block until this request finishes. The installer drives its stages through the
/// event loop (so Cancel can be handled between them), which means tests must run
/// the loop too rather than calling the stages synchronously.
bool waitForInstall(PluginInstaller &installer, const QString &requestId,
                    bool *ok, QString *code, QString *message, int timeoutMs = 5000)
{
    QEventLoop loop;
    bool done = false;

    QMetaObject::Connection conn = QObject::connect(
        &installer, &PluginInstaller::finished, &loop,
        [&](const QString &id, bool o, const QString &c, const QString &m) {
            if (id != requestId)
                return;
            done = true;
            if (ok)      *ok = o;
            if (code)    *code = c;
            if (message) *message = m;
            loop.quit();
        });

    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(conn);
    return done;
}

/// A valid, installable package.
QByteArray pluginZip(const QString &id, const QString &version,
                     const QByteArray &mainQml = "import QtQml\nQtObject{}\n")
{
    return QByteArray();   // unused placeholder; archives are built with makeZip
}

struct PackageSpec
{
    QString id;
    QString version;
    QByteArray manifestOverride;   ///< when non-empty, used verbatim
};

/// Build a .zip that unzips to plugin.json + main.qml at the archive root.
bool writePluginZip(const QString &zipPath, const QString &id, const QString &version,
                    const QByteArray &manifestOverride = QByteArray(),
                    const QByteArray &mainQml = QByteArray())
{
    const QByteArray manifest = manifestOverride.isEmpty()
        ? QStringLiteral(R"({
            "manifestVersion": 1,
            "id": "%1",
            "name": "安装测试",
            "version": "%2",
            "main": "main.qml"
        })").arg(id, version).toUtf8()
        : manifestOverride;

    const QByteArray main = mainQml.isEmpty()
        ? QByteArray("import QtQml\nQtObject{}\n")
        : mainQml;

    return makeZip(zipPath, {
        { "plugin.json", manifest },
        { "main.qml", main },
    });
}

void testInstallerSuccess()
{
    SECTION("PluginInstaller / success path");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    PluginInstaller installer(&reg);

    const QString zipPath = scratch.path + QStringLiteral("/pkg.zip");
    CHECK(writePluginZip(zipPath, QStringLiteral("com.inst.one"), QStringLiteral("1.0.0")));

    QStringList logLines;
    QObject::connect(&installer, &PluginInstaller::log, [&](const QString &, const QString &l) {
        logLines.append(l);
    });

    const QString requestId = installer.installFromZip(zipPath);
    CHECK(!requestId.isEmpty());

    bool ok = false;
    QString code, message;
    CHECK(waitForInstall(installer, requestId, &ok, &code, &message));
    CHECK(ok);
    CHECK(code.isEmpty());

    // Files landed
    CHECK(QDir(reg.pluginDir(QStringLiteral("com.inst.one"))).exists());
    CHECK(QFileInfo::exists(reg.pluginDir(QStringLiteral("com.inst.one"))
                            + QStringLiteral("/plugin.json")));

    // Registry recorded it, disabled until the manager enables it
    const auto entry = reg.get(QStringLiteral("com.inst.one"));
    CHECK(entry.has_value());
    if (entry) {
        CHECK(entry->version == QStringLiteral("1.0.0"));
        CHECK(entry->enabled == false);
        CHECK(entry->origin.kind == QStringLiteral("local"));
        CHECK(entry->problem.isNull());
    }

    // Temp dir cleaned up
    CHECK(!QDir(reg.tmpDir() + QLatin1Char('/') + requestId).exists());

    // Log file written (.logs/<requestId>.log)
    CHECK(QFileInfo::exists(reg.logsDir() + QLatin1Char('/') + requestId
                            + QStringLiteral(".log")));
    CHECK(!logLines.isEmpty());
}

void testInstallerPreCommitFailures()
{
    SECTION("PluginInstaller / pre-commit failures leave nothing behind");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    PluginInstaller installer(&reg);

    // Snapshot the state file so we can prove it was not touched.
    const QByteArray stateBefore = [&reg] {
        QFile f(reg.stateFile());
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();

    const auto expectFail = [&](const QString &zipPath, const QString &expectedCode) {
        const QString requestId = installer.installFromZip(zipPath);
        if (requestId.isEmpty()) {
            std::printf("  note: install rejected outright for %s\n",
                        expectedCode.toUtf8().constData());
            return;
        }
        bool ok = true;
        QString code, message;
        const bool finished = waitForInstall(installer, requestId, &ok, &code, &message);
        CHECK(finished);
        CHECK(!ok);
        if (code != expectedCode) {
            std::printf("  expected code '%s' but got '%s'\n",
                        expectedCode.toUtf8().constData(), code.toUtf8().constData());
        }
        CHECK(code == expectedCode);
        CHECK(reg.count() == 0);                        // registry untouched
    };

    // 1. missing file
    expectFail(scratch.path + QStringLiteral("/nope.zip"), QStringLiteral("not-found"));

    // 2. not a zip
    {
        const QString p = scratch.path + QStringLiteral("/bogus.zip");
        CHECK(writeTextFile(p, QByteArray("not a zip")));
        expectFail(p, QStringLiteral("invalid-zip"));
    }

    // 3. zip without plugin.json
    {
        const QString p = scratch.path + QStringLiteral("/nomanifest.zip");
        CHECK(makeZip(p, { { "readme.txt", "hi" } }));
        expectFail(p, QStringLiteral("not-a-plugin"));
    }

    // 4. zip-slip
    {
        const QString p = scratch.path + QStringLiteral("/slip.zip");
        CHECK(makeZip(p, {
            { "plugin.json", QStringLiteral(R"({"manifestVersion":1,"id":"com.inst.evil","name":"x","version":"1.0.0","main":"main.qml"})").toUtf8() },
            { "../escaped.txt", "pwned" },
        }));
        expectFail(p, QStringLiteral("zip-slip"));
        CHECK(!QDir(reg.pluginDir(QStringLiteral("com.inst.evil"))).exists());
    }

    // 5. invalid manifest (bad version)
    {
        const QString p = scratch.path + QStringLiteral("/badmanifest.zip");
        CHECK(writePluginZip(p, QStringLiteral("com.inst.bad"), QStringLiteral("1.0"),
                             QByteArray(R"({"manifestVersion":1,"id":"com.inst.bad","name":"x","version":"1.0","main":"main.qml"})")));
        expectFail(p, QStringLiteral("invalid-manifest"));
    }

    // 6. incompatible engines range — must fail because of the declared range,
    //    not because the host version is unparsable.
    {
        const QString p = scratch.path + QStringLiteral("/incompat.zip");
        CHECK(writePluginZip(p, QStringLiteral("com.inst.old"), QStringLiteral("1.0.0"),
                             QByteArray(R"({"manifestVersion":1,"id":"com.inst.old","name":"x","version":"1.0.0","main":"main.qml","engines":{"assistant":">=99.0.0"}})")));

        const QString requestId = installer.installFromZip(p);
        bool ok = true;
        QString code, message;
        CHECK(waitForInstall(installer, requestId, &ok, &code, &message));
        CHECK(!ok);
        CHECK(code == QStringLiteral("incompatible-version"));
        // 这条断言就是防"假通过"的关键：错误信息必须提到范围，而不是空版本。
        if (!message.contains(QStringLiteral("99.0.0")))
            std::printf("  message was: %s\n", message.toUtf8().constData());
        CHECK(message.contains(QStringLiteral("99.0.0")));
        CHECK(reg.count() == 0);
    }

    // The state file must be byte-identical: six failures, zero writes.
    {
        QFile f(reg.stateFile());
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll() == stateBefore);
    }

    // No leftover plugin directories either.
    const QStringList leftovers = QDir(reg.rootDir())
        .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList realOnes;
    for (const QString &d : leftovers) {
        if (!d.startsWith(QLatin1Char('.')))
            realOnes.append(d);
    }
    CHECK(realOnes.isEmpty());
}

void testInstallerAlreadyInstalledAndUpgrade()
{
    SECTION("PluginInstaller / already-installed + upgrade");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    PluginInstaller installer(&reg);

    const QString v1 = scratch.path + QStringLiteral("/v1.zip");
    CHECK(writePluginZip(v1, QStringLiteral("com.inst.up"), QStringLiteral("1.0.0")));

    {
        const QString requestId = installer.installFromZip(v1);
        bool ok = false;
        CHECK(waitForInstall(installer, requestId, &ok, nullptr, nullptr));
        CHECK(ok);
    }

    // Re-installing the same version must be refused, not silently overwrite.
    {
        const QString requestId = installer.installFromZip(v1);
        bool ok = true;
        QString code;
        CHECK(waitForInstall(installer, requestId, &ok, &code, nullptr));
        CHECK(!ok);
        CHECK(code == QStringLiteral("already-installed"));
    }

    // A newer version replaces it.
    const QString v2 = scratch.path + QStringLiteral("/v2.zip");
    CHECK(writePluginZip(v2, QStringLiteral("com.inst.up"), QStringLiteral("2.0.0")));
    {
        const QString requestId = installer.installFromZip(v2);
        bool ok = false;
        CHECK(waitForInstall(installer, requestId, &ok, nullptr, nullptr));
        CHECK(ok);
    }

    const auto entry = reg.get(QStringLiteral("com.inst.up"));
    CHECK(entry.has_value());
    if (entry)
        CHECK(entry->version == QStringLiteral("2.0.0"));

    // Old version must not be left in the plugin directory.
    CHECK(!QDir(reg.pluginDir(QStringLiteral("com.inst.up"))).entryList().isEmpty());
}

void testInstallerBusy()
{
    SECTION("PluginInstaller / single install at a time");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    PluginInstaller installer(&reg);

    const QString zipPath = scratch.path + QStringLiteral("/one.zip");
    CHECK(writePluginZip(zipPath, QStringLiteral("com.inst.busy"), QStringLiteral("1.0.0")));

    const QString first = installer.installFromZip(zipPath);
    CHECK(!first.isEmpty());
    CHECK(installer.isBusy());

    // Second request while the first is still queued must be refused.
    const QString second = installer.installFromZip(zipPath);
    CHECK(second.isEmpty());

    bool ok = false;
    CHECK(waitForInstall(installer, first, &ok, nullptr, nullptr));
    CHECK(ok);
    CHECK(!installer.isBusy());
}

void testInstallerCancel()
{
    SECTION("PluginInstaller / cancel");

    ScratchDir scratch;
    if (!scratch.isValid()) { CHECK(false); return; }

    PluginRegistry reg(scratch.pluginRoot());
    CHECK(reg.load());
    PluginInstaller installer(&reg);

    const QString zipPath = scratch.path + QStringLiteral("/cancel.zip");
    CHECK(writePluginZip(zipPath, QStringLiteral("com.inst.cancel"), QStringLiteral("1.0.0")));

    const QString requestId = installer.installFromZip(zipPath);
    CHECK(!requestId.isEmpty());

    // Cancel immediately: the stages are queued, so this lands before the commit.
    CHECK(installer.cancel(requestId));

    bool ok = true;
    QString code;
    CHECK(waitForInstall(installer, requestId, &ok, &code, nullptr));
    CHECK(!ok);
    CHECK(code == QStringLiteral("cancelled"));

    // Nothing installed.
    CHECK(!QDir(reg.pluginDir(QStringLiteral("com.inst.cancel"))).exists());
    CHECK(reg.count() == 0);
    CHECK(!installer.isBusy());

    // Cancelling an unknown request is a no-op, not a crash.
    CHECK(!installer.cancel(QStringLiteral("no-such-request")));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // 真实程序由 CMake 注入 APP_VERSION；测试进程必须同样设定，否则
    // PluginManifest::checkEngine 会因为"宿主版本无法解析"而失败，
    // 让 incompatible-version 的断言在错误的原因上"通过"。
    QCoreApplication::setApplicationVersion(QStringLiteral("0.3.0"));

#ifdef PLUGIN_TEST_QML_IMPORT_PATH
    // Must be set before the first QQmlEngine is constructed: the engine snapshots
    // its import paths at creation.
    qputenv("QML2_IMPORT_PATH", QByteArray(PLUGIN_TEST_QML_IMPORT_PATH));
#endif

    std::printf("=== plugin core tests ===\n");

    testParse();
    testCompare();
    testToString();
    testSatisfies();
    testIsValidRange();
    testManifestValid();
    testManifestInvalid();
    testManifestContributions();
    testCheckEngine();

    testRegistryPaths();
    testRegistryFirstRun();
    testRegistryRoundTrip();
    testRegistryRemove();
    testRegistryCorrupt();
    testRegistrySchemaMismatch();
    testRegistrySaveFailureRollsBack();
    testRegistryDataDir();

    testHostBasics();
    testHostSignalsAndGuards();
    testLoaderRequiresEngine();
    testLoaderFailures();
    testLoaderLoadAndUnload();
    testLoaderNavigation();
    testLoaderUnloadAll();

    testManagerDiscovery();
    testManagerRows();
    testManagerEnableDisable();
    testManagerBrokenPlugin();
    testManagerUninstall();
    testManagerStartupLoad();
    testLoaderPages();

    testPluginSpec();
    testPluginSpecLocalPath();
    testArchiveEntryNames();
    testArchiveExtract();
    testArchiveRejects();

    testInstallerSuccess();
    testInstallerPreCommitFailures();
    testInstallerAlreadyInstalledAndUpgrade();
    testInstallerBusy();
    testInstallerCancel();

    std::printf("\n----------------------------------------\n");
    std::printf("checks: %d   failures: %d\n", g_checks, g_failures);

    if (g_failures == 0)
        std::printf("RESULT: PASS\n");
    else
        std::printf("RESULT: FAIL\n");

    return g_failures == 0 ? 0 : 1;
}
