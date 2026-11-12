#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include "core/FileClassifier.h"
#include "core/FileRules.h"

class ConfigManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString theme READ theme NOTIFY themeChanged)
    Q_PROPERTY(int windowX READ windowX NOTIFY windowPositionChanged)
    Q_PROPERTY(int windowY READ windowY NOTIFY windowPositionChanged)
    Q_PROPERTY(QString hotkeyModifiers READ hotkeyModifiers NOTIFY hotkeyChanged)
    Q_PROPERTY(QString hotkeyKey READ hotkeyKey NOTIFY hotkeyChanged)
    Q_PROPERTY(QString logLevel READ logLevel NOTIFY logLevelChanged)
    Q_PROPERTY(bool autoStart READ autoStart NOTIFY autoStartChanged)
    Q_PROPERTY(int fileIndexHardLimitEntries READ fileIndexHardLimitEntries NOTIFY fileIndexHardLimitChanged)
    Q_PROPERTY(QString fileKindFilterMode READ fileKindFilterMode NOTIFY fileKindFilterChanged)
    Q_PROPERTY(QStringList fileKindFilterKinds READ fileKindFilterKinds NOTIFY fileKindFilterChanged)
    Q_PROPERTY(QString fileProjectPolicy READ fileProjectPolicy NOTIFY fileProjectPolicyChanged)
    Q_PROPERTY(bool inboxEnabled READ inboxEnabled NOTIFY inboxChanged)
    Q_PROPERTY(QString inboxPath READ inboxPath NOTIFY inboxChanged)
    Q_PROPERTY(int inboxSettleMs READ inboxSettleMs NOTIFY inboxChanged)
    Q_PROPERTY(QString syncLocalRoot READ syncLocalRoot NOTIFY syncChanged)
    Q_PROPERTY(QVariantList syncRoots READ syncRoots NOTIFY syncChanged)
    Q_PROPERTY(QString syncSharedDir READ syncSharedDir NOTIFY syncChanged)
    Q_PROPERTY(QString syncPeerAddress READ syncPeerAddress NOTIFY syncChanged)
    Q_PROPERTY(int syncPort READ syncPort NOTIFY syncChanged)
    Q_PROPERTY(QString syncDeviceName READ syncDeviceName NOTIFY syncChanged)
    Q_PROPERTY(int syncAutoIntervalMinutes READ syncAutoIntervalMinutes NOTIFY syncChanged)
    Q_PROPERTY(bool syncWatchFiles READ syncWatchFiles NOTIFY syncChanged)
    Q_PROPERTY(QString syncInboxDir READ syncInboxDir NOTIFY syncChanged)

public:
    explicit ConfigManager(QObject *parent = nullptr);

    // ---- load / save ----
    void load();
    void save();

    // ---- getters ----
    QString theme() const;
    int windowX() const;
    int windowY() const;
    QString hotkeyModifiers() const;
    QString hotkeyKey() const;
    QString logLevel() const;

    /// 开机启动状态。**真实性来源是注册表**（见 `core/AutoStart.h`）：
    /// 每次 `load()` 同步一次，不写进 config.json（避免两处真相不一致）。
    bool autoStart() const;

    /// 文件索引引擎：`"legacy"`（旧 FileSearcher，同步建索引）或
    /// `"v2"`（FileIndex 门面 + 后台扫描），**默认 v2**。
    ///

    /// 索引硬保护上限（架构 §3.3）：到了就**停止索引**，不驱逐已有条目。
    /// 默认 100 万；软预算（30 万）只记日志，不动作。
    int fileIndexHardLimitEntries() const;

    // ---- 文件治理（Phase B，`fileIndex.kindFilter` / `fileIndex.exclude`）----
    /// 扩展名归类器（含 `kindOverrides` 与 `extraExtensions`）。
    const FileClassifier &fileClassifier() const;
    /// 采集排除规则（目录名 / 通配 / 大小 / 工程哨兵）。
    const ExcludeRuleSet &excludeRules() const;
    /// 检索过滤白名单（改它只需重建快照，不需重扫）。
    const KindFilter &kindFilter() const;
    /// QML 用的白名单视图（设置页）：模式 + 选中的种类 token。
    QString fileKindFilterMode() const;
    QStringList fileKindFilterKinds() const;
    /// 工程目录策略（skip / demote / index）。改它需要重扫索引。
    QString fileProjectPolicy() const;

    // ---- 下载接管（Phase C，`inbox`）----
    bool    inboxEnabled() const;      ///< 默认 false：不主动搬用户文件，需显式开启
    QString inboxPath() const;         ///< 收件箱目录（空配置时用 AppConfigLocation/Inbox）
    int     inboxSettleMs() const;     ///< 稳定静默期
    // ---- 设备同步（Phase: 联网功能）----
    QString syncLocalRoot() const;             // 本地共享根（单根时代的旧字段，保留兼容）
    /// 要同步的本地文件夹列表，每项 {localPath, alias}。为空时回退到 syncLocalRoot。
    QVariantList syncRoots() const;
    QString syncSharedDir() const;             // 共享目录（网盘中转）
    QString syncPeerAddress() const;           // 直连通道对端地址（预留）
    int     syncPort() const;                  // 直连通道端口（预留）
    QString syncDeviceName() const;            // 本机名，用于冲突副本命名
    int     syncAutoIntervalMinutes() const;   // 0 = 关闭自动同步
    bool    syncWatchFiles() const;            // 本地文件变化时自动同步
    QString syncInboxDir() const;              // 投递文件落地目录（空 = 文档/Assistant 收件箱）

    // ---- setters (persist immediately) ----
    Q_INVOKABLE void setTheme(const QString &theme);
    Q_INVOKABLE void saveWindowPosition(int x, int y);
    Q_INVOKABLE void setHotkey(const QString &modifiers, const QString &key);
    Q_INVOKABLE void setLogLevel(const QString &level);
    Q_INVOKABLE void setAutoStart(bool enabled);
    Q_INVOKABLE void setFileIndexHardLimitEntries(int entries);

    /// 设置页：白名单模式 + 选中的种类的 token（如 "doc"/"img"）。
    Q_INVOKABLE void setFileKindFilter(const QString &mode, const QStringList &kinds);
    /// 设置页：工程目录策略。保存后由 main.cpp 触发 `FileIndex::refresh()`。
    Q_INVOKABLE void setFileProjectPolicy(const QString &policy);

    /// 设置页：是否接管浏览器下载（只接管开启之后的新文件）。
    Q_INVOKABLE void setInboxEnabled(bool enabled);
    Q_INVOKABLE void setInboxPath(const QString &path);

    Q_INVOKABLE void setSyncLocalRoot(const QString &path);
    Q_INVOKABLE void setSyncRoots(const QVariantList &roots);
    Q_INVOKABLE void setSyncSharedDir(const QString &path);
    Q_INVOKABLE void setSyncPeerAddress(const QString &address);
    Q_INVOKABLE void setSyncPort(int port);
    Q_INVOKABLE void setSyncDeviceName(const QString &name);
    Q_INVOKABLE void setSyncAutoIntervalMinutes(int minutes);
    Q_INVOKABLE void setSyncWatchFiles(bool enabled);
    Q_INVOKABLE void setSyncInboxDir(const QString &path);

    // ---- config file path (read-only, for debugging) ----
    QString filePath() const;

signals:
    void themeChanged();
    void windowPositionChanged();
    void hotkeyChanged();
    void logLevelChanged();
    void autoStartChanged();
    void fileIndexHardLimitChanged();
    void fileKindFilterChanged();
    void fileProjectPolicyChanged();
    void inboxChanged();
    void syncChanged();
    void configLoaded();

private:
    QString configFilePath() const;
    void applyDefaults();

    /// 把任意输入收敛成 "legacy" / "v2"（写错或旧版 config 缺这一节都算 legacy）。

    QString m_theme;
    int m_windowX = -1;
    int m_windowY = -1;
    QString m_hotkeyModifiers;
    QString m_hotkeyKey;
    QString m_logLevel;
    bool    m_autoStart = false;   // 注册表镜像，不持久化到 JSON
    int     m_fileIndexHardLimit = 1000000;   // 架构 §3.3 的硬保护上限
    FileClassifier m_fileClassifier;          // Phase B：扩展名 → 种类
    ExcludeRuleSet m_excludeRules;            // Phase B：采集排除规则
    KindFilter     m_kindFilter;              // Phase B：检索白名单
    QVariantMap    m_kindOverrides;           // Phase B：原始 kindOverrides（扩展名 → token）
    bool           m_inboxEnabled = false;    // Phase C：默认不接管（用户显式开启）
    QString        m_inboxPath;               // 空 = AppConfigLocation/Inbox
    int            m_inboxSettleMs = 1200;
    QString m_filePath;

    QString m_syncLocalRoot;
    QVariantList m_syncRoots;
    QString m_syncSharedDir;
    QString m_syncPeerAddress;
    int     m_syncPort = 45871;
    QString m_syncDeviceName;
    int     m_syncAutoIntervalMinutes = 0;
    bool    m_syncWatchFiles = true;
    QString m_syncInboxDir;
};
