#include "ui/AuthBridge.h"

#include "core/Logger.h"
#include "file_guard/crypto.h"
#include "file_guard/util.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <filesystem>

namespace {

std::filesystem::path authDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return std::filesystem::path((dir + QStringLiteral("/.auth")).toStdWString());
}

std::filesystem::path accountPath()
{
    return authDir() / "account.dat";
}

// 旧版本位置：security/account.json（明文 JSON），首次启动自动迁移
std::filesystem::path legacyAccountPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return std::filesystem::path((dir + QStringLiteral("/security/account.json")).toStdWString());
}

QJsonObject readAccountObject()
{
    try {
        const auto path = accountPath();
        if (std::filesystem::exists(path)) {
            const auto encrypted = file_guard::util::readAll(path);
            const auto plain = file_guard::util::dpapiUnprotect(encrypted);
            const auto doc = QJsonDocument::fromJson(
                QByteArray(reinterpret_cast<const char *>(plain.data()),
                           static_cast<int>(plain.size())));
            if (doc.isObject())
                return doc.object();
            return {};
        }
        const auto legacy = legacyAccountPath();
        if (std::filesystem::exists(legacy)) {
            const auto raw = file_guard::util::readAll(legacy);
            const auto doc = QJsonDocument::fromJson(
                QByteArray(reinterpret_cast<const char *>(raw.data()),
                           static_cast<int>(raw.size())));
            if (doc.isObject())
                return doc.object();
        }
    } catch (...) {
    }
    return {};
}

void writeAccountObject(const QJsonObject &obj)
{
    std::error_code ignored;
    std::filesystem::create_directories(authDir(), ignored);
    const auto plain = QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
    file_guard::crypto::Bytes bytes(plain.begin(), plain.end());
    const auto encrypted = file_guard::util::dpapiProtect(bytes);
    file_guard::util::writeAll(accountPath(), encrypted);
    file_guard::util::restrictAcl(accountPath());
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
        saveAccount(QStringLiteral("hacker"), QStringLiteral("hacked123"));
        const bool overwriteRejected =
            verifyPassword(QStringLiteral("demo"), QStringLiteral("Passw0rd123"));
        changePassword(QStringLiteral("Passw0rd123"), QStringLiteral("NewPassw0rd!"));
        const bool changedOk = verifyPassword(QStringLiteral("demo"), QStringLiteral("NewPassw0rd!"));
        Logger::get().info(
            "AuthBridge selftest: login_ok={} wrong_rejected={} overwrite_rejected={} change_ok={}",
            loginOk, wrongRejected, overwriteRejected, changedOk);
        logout();
        std::error_code ignored;
        std::filesystem::remove(accountPath(), ignored);
        std::filesystem::remove(legacyAccountPath(), ignored);
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
    const auto obj = readAccountObject();
    m_hasAccount = false;
    m_signedIn = false;
    m_user.clear();

    const QString user = obj.value(QStringLiteral("user")).toString();
    const QString hash = obj.value(QStringLiteral("hash")).toString();
    if (!user.isEmpty() && hash.size() == 64) {
        m_hasAccount = true;
        m_user = user;
        m_signedIn = obj.value(QStringLiteral("signedIn")).toBool();
    }

    // 旧位置迁移到 DPAPI 加密的新位置，并删除明文旧文件
    if (m_hasAccount && std::filesystem::exists(legacyAccountPath())
        && !std::filesystem::exists(accountPath())) {
        try {
            writeAccountObject(obj);
            std::error_code ignored;
            std::filesystem::remove(legacyAccountPath(), ignored);
            Logger::get().info("AuthBridge: migrated account storage to DPAPI");
        } catch (...) {
        }
    }

    setStatus(m_signedIn ? QStringLiteral("账号：已登录（%1）").arg(m_user)
                         : (m_hasAccount ? QStringLiteral("账号：未登录（可离线登录）")
                                         : QStringLiteral("账号：未设置（离线模式受限）")));
    emit changed();
}

void AuthBridge::saveSession(bool signedIn)
{
    auto obj = readAccountObject();
    if (obj.isEmpty())
        return;
    obj.insert(QStringLiteral("signedIn"), signedIn);
    try {
        writeAccountObject(obj);
    } catch (...) {
    }
}

bool AuthBridge::verifyPassword(const QString &user, const QString &password) const
{
    const auto obj = readAccountObject();
    if (obj.isEmpty())
        return false;
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

    unsigned char diff = 0;
    for (int i = 0; i < expected.size(); ++i)
        diff |= static_cast<unsigned char>(expected.at(i).toLatin1() != actualHex.at(i));
    return diff == 0;
}

void AuthBridge::login(const QString &user, const QString &password)
{
    if (!m_hasAccount) {
        setStatus(QStringLiteral("本机还没有账号，请到 设置 → 账号 创建"));
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
    setStatus(QStringLiteral("账号：已登录（%1）").arg(m_user));
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
    // 安全：已有账号时禁止“保存账号”覆盖（否则可绕过旧密码校验）。
    // 修改密码必须走 changePassword（校验旧密码）；换账号请用「切换账号」。
    if (m_hasAccount) {
        setStatus(QStringLiteral("账号已存在：修改密码请用「更换密码」，登录请用「登录」"));
        emit toast(QStringLiteral("已存在账号，不能直接覆盖"));
        Logger::get().warn("AuthBridge: saveAccount blocked — account already exists");
        return;
    }

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
    obj.insert(QStringLiteral("version"), 2);
    obj.insert(QStringLiteral("user"), name);
    obj.insert(QStringLiteral("salt"), QString::fromStdString(file_guard::util::hexEncode(salt)));
    obj.insert(QStringLiteral("iterations"), 100'000);
    obj.insert(QStringLiteral("hash"), QString::fromStdString(file_guard::util::hexEncode(hash)));
    obj.insert(QStringLiteral("signedIn"), true);
    writeAccountObject(obj);

    m_hasAccount = true;
    m_signedIn = true;
    m_offline = false;
    m_user = name;
    setStatus(QStringLiteral("账号：已保存并登录（%1）").arg(name));
    Logger::get().info("AuthBridge: account saved user={}", name.toStdString());
    emit toast(QStringLiteral("账号已保存"));
    emit changed();
}

void AuthBridge::changePassword(const QString &oldPassword, const QString &newPassword)
{
    if (!m_signedIn) {
        setStatus(QStringLiteral("请先登录再更换密码"));
        emit toast(QStringLiteral("请先登录"));
        return;
    }
    if (newPassword.size() < 6) {
        setStatus(QStringLiteral("新密码至少 6 位"));
        emit toast(QStringLiteral("新密码至少 6 位"));
        return;
    }
    if (!verifyPassword(m_user, oldPassword)) {
        setStatus(QStringLiteral("当前密码错误"));
        emit toast(QStringLiteral("当前密码错误"));
        return;
    }

    auto obj = readAccountObject();
    if (obj.isEmpty()) {
        setStatus(QStringLiteral("账号数据读取失败"));
        return;
    }
    const auto salt = file_guard::crypto::randomBytes(16);
    const auto hash = file_guard::crypto::pbkdf2(newPassword.toStdString(), salt, 100'000, 32);
    obj.insert(QStringLiteral("version"), 2);
    obj.insert(QStringLiteral("salt"), QString::fromStdString(file_guard::util::hexEncode(salt)));
    obj.insert(QStringLiteral("iterations"), 100'000);
    obj.insert(QStringLiteral("hash"), QString::fromStdString(file_guard::util::hexEncode(hash)));
    obj.insert(QStringLiteral("signedIn"), true);
    try {
        writeAccountObject(obj);
        setStatus(QStringLiteral("密码已更新"));
        Logger::get().info("AuthBridge: password changed user={}", m_user.toStdString());
        emit toast(QStringLiteral("密码已更新"));
        emit changed();
    } catch (const std::exception &ex) {
        setStatus(QStringLiteral("保存失败：%1").arg(QString::fromUtf8(ex.what())));
    }
}

void AuthBridge::changeUserName(const QString &newName)
{
    if (!m_signedIn) {
        setStatus(QStringLiteral("请先登录再更改用户名"));
        emit toast(QStringLiteral("请先登录"));
        return;
    }
    const QString name = newName.trimmed();
    if (name.isEmpty()) {
        setStatus(QStringLiteral("用户名不能为空"));
        emit toast(QStringLiteral("用户名不能为空"));
        return;
    }
    if (name == m_user) {
        setStatus(QStringLiteral("新用户名与当前相同"));
        return;
    }

    auto obj = readAccountObject();
    if (obj.isEmpty()) {
        setStatus(QStringLiteral("账号数据读取失败"));
        return;
    }
    obj.insert(QStringLiteral("user"), name);
    obj.insert(QStringLiteral("signedIn"), true);
    try {
        writeAccountObject(obj);
        m_user = name;
        setStatus(QStringLiteral("账号：已登录（%1）").arg(name));
        Logger::get().info("AuthBridge: username changed to {}", name.toStdString());
        emit toast(QStringLiteral("用户名已更改"));
        emit changed();
    } catch (const std::exception &ex) {
        setStatus(QStringLiteral("保存失败：%1").arg(QString::fromUtf8(ex.what())));
    }
}

void AuthBridge::switchAccount()
{
    m_signedIn = false;
    m_offline = false;
    saveSession(false);
    setStatus(QStringLiteral("请登录其他账号（本机账号已保留）"));
    Logger::get().info("AuthBridge: switch account requested");
    emit toast(QStringLiteral("请登录其他账号"));
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
