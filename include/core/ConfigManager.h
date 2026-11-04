#pragma once

#include <QObject>
#include <QString>

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
    Q_PROPERTY(QString fileIndexEngine READ fileIndexEngine NOTIFY fileIndexEngineChanged)
    Q_PROPERTY(int fileIndexHardLimitEntries READ fileIndexHardLimitEntries NOTIFY fileIndexHardLimitChanged)

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
    /// 这是 A2 的 **A/B 回退开关**（架构文档 §7）：v2 出任何异常都能一行配置退回去，
    /// 旧 `FileSearcher` 在 A7 之前**完全保留**。取值非法时按 `"legacy"` 处理。
    QString fileIndexEngine() const;

    /// 索引硬保护上限（架构 §3.3）：到了就**停止索引**，不驱逐已有条目。
    /// 默认 100 万；软预算（30 万）只记日志，不动作。
    int fileIndexHardLimitEntries() const;

    // ---- setters (persist immediately) ----
    Q_INVOKABLE void setTheme(const QString &theme);
    Q_INVOKABLE void saveWindowPosition(int x, int y);
    Q_INVOKABLE void setHotkey(const QString &modifiers, const QString &key);
    Q_INVOKABLE void setLogLevel(const QString &level);
    Q_INVOKABLE void setAutoStart(bool enabled);
    Q_INVOKABLE void setFileIndexEngine(const QString &engine);
    Q_INVOKABLE void setFileIndexHardLimitEntries(int entries);

    // ---- config file path (read-only, for debugging) ----
    QString filePath() const;

signals:
    void themeChanged();
    void windowPositionChanged();
    void hotkeyChanged();
    void logLevelChanged();
    void autoStartChanged();
    void fileIndexEngineChanged();
    void fileIndexHardLimitChanged();
    void configLoaded();

private:
    QString configFilePath() const;
    void applyDefaults();

    /// 把任意输入收敛成 "legacy" / "v2"（写错或旧版 config 缺这一节都算 legacy）。
    static QString normalizedEngine(const QString &raw);

    QString m_theme;
    int m_windowX = -1;
    int m_windowY = -1;
    QString m_hotkeyModifiers;
    QString m_hotkeyKey;
    QString m_logLevel;
    bool    m_autoStart = false;   // 注册表镜像，不持久化到 JSON
    QString m_fileIndexEngine;     // "legacy" | "v2"（A2 的 A/B 开关）
    int     m_fileIndexHardLimit = 1000000;   // 架构 §3.3 的硬保护上限
    QString m_filePath;
};
