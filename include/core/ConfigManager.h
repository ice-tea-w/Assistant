#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

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
    // ---- 设备同步（Phase: 联网功能）----
    QString syncLocalRoot() const;             // 本地共享根（单根时代的旧字段，保留兼容）
    /// 要同步的本地文件夹列表，每项 {localPath, alias}。为空时回退到 syncLocalRoot。
    QVariantList syncRoots() const;
    QString syncSharedDir() const;             // 共享目录（网盘中转）
    QString syncPeerAddress() const;           // 直连通道对端地址（配对成功后的主机）
    int     syncPort() const;                  // 直连通道端口
    QString syncDeviceId() const;              // 本机稳定标识（首次配对时生成并写盘）
    QString syncPeerFingerprint() const;       // 已配对主机的证书指纹（空 = 未配对）
    QString syncPeerSecret() const;            // 主机下发的设备口令，Base64（空 = 未配对）
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

    Q_INVOKABLE void setSyncLocalRoot(const QString &path);
    Q_INVOKABLE void setSyncRoots(const QVariantList &roots);
    Q_INVOKABLE void setSyncSharedDir(const QString &path);
    Q_INVOKABLE void setSyncPeerAddress(const QString &address);
    Q_INVOKABLE void setSyncPort(int port);
    Q_INVOKABLE void setSyncDeviceId(const QString &id);
    Q_INVOKABLE void setSyncPeerFingerprint(const QString &fingerprint);
    Q_INVOKABLE void setSyncPeerSecret(const QString &secretBase64);
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
    QString m_filePath;

    QString m_syncLocalRoot;
    QVariantList m_syncRoots;
    QString m_syncSharedDir;
    QString m_syncPeerAddress;
    int     m_syncPort = 45871;
    QString m_syncDeviceId;
    QString m_syncPeerFingerprint;
    QString m_syncPeerSecret;
    QString m_syncDeviceName;
    int     m_syncAutoIntervalMinutes = 1;
    bool    m_syncWatchFiles = true;
    QString m_syncInboxDir;
};
