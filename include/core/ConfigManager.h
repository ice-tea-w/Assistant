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

    // ---- setters (persist immediately) ----
    Q_INVOKABLE void setTheme(const QString &theme);
    Q_INVOKABLE void saveWindowPosition(int x, int y);
    Q_INVOKABLE void setHotkey(const QString &modifiers, const QString &key);
    Q_INVOKABLE void setLogLevel(const QString &level);

    // ---- config file path (read-only, for debugging) ----
    QString filePath() const;

signals:
    void themeChanged();
    void windowPositionChanged();
    void hotkeyChanged();
    void logLevelChanged();
    void configLoaded();

private:
    QString configFilePath() const;
    void applyDefaults();

    QString m_theme;
    int m_windowX = -1;
    int m_windowY = -1;
    QString m_hotkeyModifiers;
    QString m_hotkeyKey;
    QString m_logLevel;
    QString m_filePath;
};
