#include "ui/AuthBridge.h"

#include "core/Logger.h"
#include "file_guard/crypto.h"
#include "file_guard/util.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <filesystem>

namespace {

std::filesystem::path accountPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return std::filesystem::path(
        (dir + QStringLiteral("/security/account.json")).toStdWString());
}

} // namespace

AuthBridge::AuthBridge(QObject *parent)
    : QObject(parent)
{
    load();

    if (qEnvironmentVariableIsSet("FG_AUTH_SELFTEST")) {
        saveAccount(QStringLiteral("demo"), QStringLiteral("Passw0rd123"));
        const bool loginOk = verifyPassword(QStringLiteral("demo"), QStringLiteral("Passw0rd123"));
        const bool wrongRejected = !verifyPassword(QStringLiteral("demo"), QStringLiteral("bad"));
        logout();
        Logger::get().info("AuthBridge selftest: login_ok={} wrong_rejected={} has_account={}",
                           loginOk, wrongRejected, hasAccount());
        std::error_code ignored;
        std::filesystem::remove(accountPath(), ignored);
        m_hasAccount = false;
        m_user.clear();
        m_signedIn = false;
        m_offline = false;
        setStatus(QString());
    } else if (qEnvironmentVariableIsSet("FG_AUTH_SETUP")) {
        const QString spec = qEnvironmentVariable("FG_AUTH_SETUP");
        const int sep = spec.indexOf(QLatin1Char(':'));
        if (sep > 0) {
            saveAccount(spec.left(sep), spec.mid(sep + 1));
            logout();
        }
    } else if (qEnvironmentVariableIsSet("FG_AUTH_OFFLINE")) {
        continueOffline();
    } else if (qEnvironmentVariableIsSet("FG_AUTH_LOGIN")) {
        const QString spec = qEnvironmentVariable("FG_AUTH_LOGIN");
        const int sep = spec.indexOf(QLatin1Char(':'));
        if (sep > 0)
            login(spec.left(sep), spec.mid(sep + 1));
    }
}

void AuthBridge::load()
{
    const auto path = accountPath();
    QFile file(QString::fromStdWString(path.wstring()));
    m_hasAccount = false;
    m_signedIn = false;
    m_user.clear();

    if (file.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (doc.isObject()) {
            const auto obj = doc.object();
            const QString user = obj.value(QStringLiteral("user")).toString();
            const QString hash = obj.value(QStringLiteral("hash")).toString();
            if (!user.isEmpty() && hash.size() == 64) {
                m_hasAccount = true;
                m_user = user;
                m_signedIn = obj.value(QStringLiteral("signedIn")).toBool();
            }
        }
    }

    setStatus(m_signedIn ? QStringLiteral("安全中心账号：已登录（%1）").arg(m_user)
                         : QStringLiteral("安全中心账号：未登录（离线模式部分功能受限）"));
    emit changed();
}

void AuthBridge::saveSession(bool signedIn) const
{
    const auto path = accountPath();
    QFile file(QString::fromStdWString(path.wstring()));
    if (file.open(QIODevice::ReadOnly)) {
        auto doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (!doc.isObject())
            return;
        auto obj = doc.object();
        obj.insert(QStringLiteral("signedIn"), signedIn);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
            file.close();
        }
    }
}

bool AuthBridge::verifyPassword(const QString &user, const QString &password) const
{
    const auto path = accountPath();
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const auto doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject())
        return false;

    const auto obj = doc.object();
    if (obj.value(QStringLiteral("user")).toString() != user)
        return false;

    const auto fromHex = [](const QString &hex) {
        file_guard::crypto::Bytes out;
        out.reserve(static_cast<std::size_t>(hex.size() / 2));
        for (int i = 0; i + 1 < hex.size(); i += 2)
            out.push_back(static_cast<unsigned char>(hex.mid(i, 2).toUInt(nullptr, 16)));
        return out;
    };

    const auto salt = fromHex(obj.value(QStringLiteral("salt")).toString());
    const auto expected = obj.value(QStringLiteral("hash")).toString();
    if (salt.empty() || expected.size() != 64)
        return false;

    const unsigned iterations =
        static_cast<unsigned>(obj.value(QStringLiteral("iterations")).toInt(100'000));
    const auto actual = file_guard::crypto::pbkdf2(password.toStdString(), salt, iterations, 32);
    const auto actualHex = QString::fromStdString(file_guard::util::hexEncode(actual));

    // 常量时间比较，避免按字节提前返回
    unsigned char diff = 0;
    for (int i = 0; i < expected.size(); ++i)
        diff |= static_cast<unsigned char>(expected.at(i).toLatin1() != actualHex.at(i));
    return diff == 0;
}

void AuthBridge::login(const QString &user, const QString &password)
{
    if (!m_hasAccount) {
        setStatus(QStringLiteral("本机还没有账号，请先到 安全防护 → 账号与登录 创建"));
        emit toast(QStringLiteral("请先在设置中创建账号"));
        return;
    }
    const QString trimmed = user.trimmed();
    if (trimmed.isEmpty() || password.isEmpty()) {
        setStatus(QStringLiteral("请输入账号和密码"));
        return;
    }
    if (!verifyPassword(trimmed, password)) {
        setStatus(QStringLiteral("账号或密码错误"));
        emit toast(QStringLiteral("登录失败：账号或密码错误"));
        return;
    }

    m_signedIn = true;
    m_offline = false;
    m_user = trimmed;
    saveSession(true);
    setStatus(QStringLiteral("安全中心账号：已登录（%1）").arg(m_user));
    Logger::get().info("AuthBridge: login ok user={}", m_user.toStdString());
    emit toast(QStringLiteral("登录成功"));
    emit changed();
}

void AuthBridge::continueOffline()
{
    m_offline = true;
    m_signedIn = false;
    setStatus(QStringLiteral("离线模式：设备同步、插件市场等受限"));
    Logger::get().info("AuthBridge: continue offline");
    emit toast(QStringLiteral("已进入离线模式，部分功能受限"));
    emit changed();
}

void AuthBridge::requestLogin()
{
    m_offline = false;
    m_signedIn = false;
    setStatus(QStringLiteral("请登录后使用完整功能"));
    emit changed();
}

void AuthBridge::saveAccount(const QString &user, const QString &password)
{
    const QString name = user.trimmed();
    if (name.isEmpty()) {
        setStatus(QStringLiteral("账号不能为空"));
        emit toast(QStringLiteral("请填写账号"));
        return;
    }
    if (password.size() < 6) {
        setStatus(QStringLiteral("密码至少 6 位"));
        emit toast(QStringLiteral("密码至少 6 位"));
        return;
    }

    const auto salt = file_guard::crypto::randomBytes(16);
    const auto hash = file_guard::crypto::pbkdf2(password.toStdString(), salt, 100'000, 32);

    QJsonObject obj;
    obj.insert(QStringLiteral("version"), 1);
    obj.insert(QStringLiteral("user"), name);
    obj.insert(QStringLiteral("salt"), QString::fromStdString(file_guard::util::hexEncode(salt)));
    obj.insert(QStringLiteral("iterations"), 100'000);
    obj.insert(QStringLiteral("hash"), QString::fromStdString(file_guard::util::hexEncode(hash)));
    obj.insert(QStringLiteral("signedIn"), true);

    const auto path = accountPath();
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    file_guard::util::writeText(path, QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString());
    file_guard::util::restrictAcl(path);

    m_hasAccount = true;
    m_signedIn = true;
    m_offline = false;
    m_user = name;
    setStatus(QStringLiteral("安全中心账号已保存并登录：%1").arg(name));
    Logger::get().info("AuthBridge: account saved user={}", name.toStdString());
    emit toast(QStringLiteral("账号已保存"));
    emit changed();
}

void AuthBridge::logout()
{
    m_signedIn = false;
    m_offline = false;
    saveSession(false);
    setStatus(QStringLiteral("已退出登录"));
    Logger::get().info("AuthBridge: logout");
    emit toast(QStringLiteral("已退出登录"));
    emit changed();
}

void AuthBridge::setStatus(const QString &text)
{
    if (m_status == text)
        return;
    m_status = text;
    emit changed();
}
