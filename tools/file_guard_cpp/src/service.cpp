#include "file_guard/service.h"

#include "file_guard/audit.h"
#include "file_guard/backup.h"
#include "file_guard/crypto.h"
#include "file_guard/integrity.h"
#include "file_guard/scan.h"
#include "file_guard/util.h"
#include "file_guard/vault.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <cstdint>
#include <map>
#include <sstream>

namespace file_guard {

namespace {

std::string upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return text;
}

std::string jsonField(const std::string &body, const std::string &key)
{
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t pos = body.find(needle);
    if (pos == std::string::npos)
        return {};
    std::string value;
    for (std::size_t i = pos + needle.size(); i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            value.push_back(body[i]);
            value.push_back(body[++i]);
            continue;
        }
        if (body[i] == '"')
            break;
        value.push_back(body[i]);
    }
    return util::jsonUnescape(value);
}

bool jsonFlag(const std::string &body, const std::string &key)
{
    const std::string needle = "\"" + key + "\":";
    const std::size_t pos = body.find(needle);
    if (pos == std::string::npos)
        return false;
    return body.compare(pos + needle.size(), 4, "true") == 0;
}

std::uint64_t jsonNumber(const std::string &body, const std::string &key)
{
    const std::string needle = "\"" + key + "\":";
    const std::size_t pos = body.find(needle);
    if (pos == std::string::npos)
        return 0;
    return std::strtoull(body.c_str() + pos + needle.size(), nullptr, 10);
}

std::string okJson(const std::string &result)
{
    return "{\"ok\":true,\"result\":" + result + "}";
}

std::string errorJson(int code, const std::string &message)
{
    return "{\"ok\":false,\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" +
           util::jsonEscape(message) + "\"}}";
}

std::string passwordFromEnv()
{
    return util::envOr("FG_PASSWORD", "");
}

} // namespace


// ---------------------------------------------------------------------------
// 异步任务队列（job.submit / job.status / job.list / job.cancel）
// ---------------------------------------------------------------------------
struct JobState {
    std::string id;
    std::string op;
    std::string state;   // queued / running / done / error / cancelled
    int progress = 0;
    std::string result;
    std::string error;
    bool cancelRequested = false;
};

class JobManager {
public:
    JobManager()
    {
        m_worker = std::thread([this]() { workerLoop(); });
    }

    ~JobManager()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable())
            m_worker.join();
    }

    std::string submit(const std::string &op, std::function<std::string(std::string *)> task)
    {
        const std::string id = util::randomHex(8);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_jobs.size() >= 64)
                return {};
            JobState job;
            job.id = id;
            job.op = op;
            job.state = "queued";
            m_jobs[id] = job;
            m_queue.push_back({id, std::move(task)});
        }
        m_cv.notify_one();
        return id;
    }

    bool status(const std::string &id, JobState *out)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_jobs.find(id);
        if (it == m_jobs.end())
            return false;
        *out = it->second;
        return true;
    }

    std::string listJson()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string items;
        for (const auto &item : m_jobs) {
            if (!items.empty())
                items += ",";
            items += "{\"id\":\"" + item.second.id + "\",\"op\":\"" +
                     util::jsonEscape(item.second.op) + "\",\"state\":\"" +
                     item.second.state + "\",\"progress\":" +
                     std::to_string(item.second.progress) + "}";
        }
        return "[" + items + "]";
    }

    bool cancel(const std::string &id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_jobs.find(id);
        if (it == m_jobs.end())
            return false;
        if (it->second.state == "queued") {
            it->second.state = "cancelled";
            return true;
        }
        if (it->second.state == "running") {
            it->second.cancelRequested = true;
            return true;
        }
        return false;
    }

private:
    struct QueuedTask {
        std::string id;
        std::function<std::string(std::string *)> task;
    };

    void workerLoop()
    {
        while (true) {
            QueuedTask task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty())
                    return;
                task = std::move(m_queue.front());
                m_queue.pop_front();
                auto &job = m_jobs[task.id];
                if (job.state == "cancelled")
                    continue;
                job.state = "running";
                job.progress = 10;
            }

            std::string error;
            std::string result;
            try {
                result = task.task(&error);
            } catch (const std::exception &ex) {
                error = ex.what();
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            auto &job = m_jobs[task.id];
            job.progress = 100;
            if (job.cancelRequested) {
                job.state = "cancelled";
            } else if (!error.empty()) {
                job.state = "error";
                job.error = error;
            } else {
                job.state = "done";
                job.result = result;
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<QueuedTask> m_queue;
    std::map<std::string, JobState> m_jobs;
    std::thread m_worker;
    bool m_stop = false;
};

LocalService::LocalService(Config config, ServiceOptions options)
    : m_config(std::move(config))
    , m_options(options)
    , m_jobs(std::make_unique<JobManager>())
{
    m_config.ensureDirs();
}

LocalService::~LocalService() = default;

std::string LocalService::ensureToken(const std::filesystem::path &tokenFile)
{
    const std::string existing = util::trim(util::readText(tokenFile));
    if (!existing.empty())
        return existing;
    const std::string token = util::randomHex(32);
    util::writeTextAtomic(tokenFile, token + "\n");
    util::restrictAcl(tokenFile);
    return token;
}

int LocalService::run()
{
    m_token = ensureToken(m_config.tokenFile());

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup 失败\n";
        return 1;
    }

    const SOCKET server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }
    const BOOL reuse = TRUE;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(m_options.port));
    address.sin_addr.s_addr = m_options.allowRemote ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    if (::bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        std::cerr << "端口绑定失败：" << m_options.port << "\n";
        ::closesocket(server);
        WSACleanup();
        return 1;
    }
    ::listen(server, 16);
    std::cout << "{\"listening\":true,\"host\":\"" << (m_options.allowRemote ? "0.0.0.0" : "127.0.0.1")
              << "\",\"port\":" << m_options.port << "}" << std::endl;

    int handled = 0;
    while (true) {
        SOCKET client = ::accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;
        const DWORD timeoutMs = 30000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
        std::string error;
        const std::string response = handleConnection(client, &error);
        const std::string http =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(response.size()) +
            "\r\nConnection: close\r\n\r\n" + response;
        ::send(client, http.data(), static_cast<int>(http.size()), 0);
        ::shutdown(client, SD_BOTH);
        ::closesocket(client);
        ++handled;
        if (m_options.once && handled >= 1)
            break;
    }

    ::closesocket(server);
    WSACleanup();
    return 0;
}

std::string LocalService::handleConnection(unsigned long long socketHandle, std::string *error)
{
    const SOCKET client = static_cast<SOCKET>(socketHandle);
    std::string request;
    char buffer[4096];
    std::size_t headerEnd = std::string::npos;
    while ((headerEnd = request.find("\r\n\r\n")) == std::string::npos) {
        const int received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            *error = "请求读取失败";
            return errorJson(400, "bad request");
        }
        request.append(buffer, static_cast<std::size_t>(received));
        if (request.size() > 1 << 20) {
            *error = "请求过大";
            return errorJson(413, "request too large");
        }
    }

    const std::string headers = request.substr(0, headerEnd);
    const std::string bodyPart = request.substr(headerEnd + 4);
    std::size_t contentLength = 0;
    std::string host;
    bool hasOrigin = false;
    std::string tokenHeader;
    for (const std::string &line : util::splitLines(headers)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = upper(util::trim(line.substr(0, colon)));
        const std::string value = util::trim(line.substr(colon + 1));
        if (key == "CONTENT-LENGTH")
            contentLength = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        else if (key == "HOST")
            host = value;
        else if (key == "ORIGIN")
            hasOrigin = true;
        else if (key == "X-FG-TOKEN")
            tokenHeader = value;
    }

    std::string body = bodyPart;
    while (body.size() < contentLength) {
        const int received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;
        body.append(buffer, static_cast<std::size_t>(received));
    }

    if (!m_options.allowRemote) {
        std::string hostOnly = host;
        const std::size_t colon = hostOnly.find(':');
        if (colon != std::string::npos)
            hostOnly = hostOnly.substr(0, colon);
        if (hostOnly != "127.0.0.1" && hostOnly != "localhost" && hostOnly != "::1") {
            *error = "Host 非法";
            return errorJson(403, "host not allowed");
        }
    }
    if (hasOrigin) {
        *error = "拒绝带 Origin 的请求";
        return errorJson(403, "origin not allowed");
    }
    if (tokenHeader.empty() ||
        !crypto::constantTimeEquals(crypto::Bytes(tokenHeader.begin(), tokenHeader.end()),
                                    crypto::Bytes(m_token.begin(), m_token.end()))) {
        *error = "token 校验失败";
        return errorJson(401, "invalid token");
    }

    std::string dispatchError;
    const std::string result = dispatch(body, &dispatchError);
    if (!dispatchError.empty())
        return errorJson(1, dispatchError);
    return result;
}

std::string LocalService::dispatch(const std::string &body, std::string *error)
{
    const std::string method = jsonField(body, "method");
    const std::string password = passwordFromEnv();
    if (method.empty()) {
        *error = "缺少 method";
        return {};
    }

    try {
        if (method == "ping")
            return okJson("{\"pong\":true}");
        if (method == "info")
            return okJson("{\"home\":\"" + util::jsonEscape(util::pathUtf8(m_config.home)) +
                          "\",\"port\":" + std::to_string(m_options.port) + "}");
        if (method == "encrypt") {
            if (password.empty())
                throw SecurityError(ErrorCode::Password, "服务端未设置 FG_PASSWORD");
            const std::string src = jsonField(body, "src");
            const std::string dst = jsonField(body, "dst");
            const crypto::Result result =
                crypto::encryptFile(src, dst, password, m_config.iterations, m_config.chunkSize);
            return okJson("{\"sha256\":\"" + util::hexEncode(result.sha256) +
                          "\",\"size\":" + std::to_string(result.plaintextSize) + "}");
        }
        if (method == "decrypt") {
            if (password.empty())
                throw SecurityError(ErrorCode::Password, "服务端未设置 FG_PASSWORD");
            const std::string src = jsonField(body, "src");
            const std::string dst = jsonField(body, "dst");
            const crypto::Result result = crypto::decryptFile(src, dst, password);
            return okJson("{\"sha256\":\"" + util::hexEncode(result.sha256) +
                          "\",\"size\":" + std::to_string(result.plaintextSize) + "}");
        }
        if (method == "verify") {
            if (password.empty())
                throw SecurityError(ErrorCode::Password, "服务端未设置 FG_PASSWORD");
            const crypto::Result result = crypto::verifyFile(jsonField(body, "src"), password);
            return okJson("{\"sha256\":\"" + util::hexEncode(result.sha256) + "\"}");
        }
        if (method == "scan") {
            const scan::Report report = scan::scanPath(jsonField(body, "path"), true);
            return okJson("{\"files\":" + std::to_string(report.filesScanned) +
                          ",\"high\":" + std::to_string(report.high) +
                          ",\"medium\":" + std::to_string(report.medium) +
                          ",\"low\":" + std::to_string(report.low) + "}");
        }
        if (method == "backup.add") {
            if (password.empty())
                throw SecurityError(ErrorCode::Password, "服务端未设置 FG_PASSWORD");
            BackupStore store(m_config.backupDir(), password, m_config.iterations,
                              m_config.logIterations);
            const BackupEntry entry = store.backupFile(jsonField(body, "path"));
            return okJson("{\"version\":" + std::to_string(entry.version) + ",\"sha256\":\"" +
                          entry.sha256 + "\"}");
        }
        if (method == "backup.list") {
            if (password.empty())
                throw SecurityError(ErrorCode::Password, "服务端未设置 FG_PASSWORD");
            BackupStore store(m_config.backupDir(), password, m_config.iterations,
                              m_config.logIterations);
            std::string items;
            for (const auto &entry : store.list()) {
                if (!items.empty())
                    items += ",";
                items += "{\"path\":\"" + util::jsonEscape(entry.path) + "\",\"version\":" +
                         std::to_string(entry.version) + ",\"size\":" +
                         std::to_string(entry.size) + "}";
            }
            return okJson("{\"items\":[" + items + "]}");
        }
        if (method == "backup.restore") {
            if (password.empty())
                throw SecurityError(ErrorCode::Password, "服务端未设置 FG_PASSWORD");
            BackupStore store(m_config.backupDir(), password, m_config.iterations,
                              m_config.logIterations);
            const BackupStore::RestoreResult result = store.restore(
                jsonField(body, "path"), static_cast<std::uint32_t>(jsonNumber(body, "version")),
                jsonField(body, "dest"), jsonFlag(body, "force"));
            return okJson("{\"restored\":\"" + util::jsonEscape(result.restoredTo) + "\"}");
        }
        if (method == "log.record") {
            if (password.empty())
                throw SecurityError(ErrorCode::Password, "服务端未设置 FG_PASSWORD");
            audit::AuditLog log(m_config.auditDir(), password, m_config.logIterations);
            log.append(jsonField(body, "event"), jsonField(body, "actor"), jsonField(body, "detail"));
            return okJson("{\"recorded\":true}");
        }
        if (method == "log.verify") {
            if (password.empty())
                throw SecurityError(ErrorCode::Password, "服务端未设置 FG_PASSWORD");
            audit::AuditLog log(m_config.auditDir(), password, m_config.logIterations);
            const audit::VerifyResult result = log.verify();
            return okJson("{\"ok\":" + std::string(result.ok ? "true" : "false") +
                          ",\"records\":" + std::to_string(result.records) + "}");
        }
        if (method == "vault.info") {
            Vault vault(m_config.vaultDir(), m_config.iterations, m_config.logIterations);
            return okJson("{\"exists\":" + std::string(vault.exists() ? "true" : "false") + "}");
        }
        if (method == "doctor") {
            return okJson("{\"home\":\"" + util::jsonEscape(util::pathUtf8(m_config.home)) +
                          "\",\"auditDir\":\"" + util::jsonEscape(util::pathUtf8(m_config.auditDir())) +
                          "\"}");
        }
        if (method == "job.submit") {
            const std::string op = jsonField(body, "op");
            static const char *allowed[] = {
                "ping", "info", "encrypt", "decrypt", "verify", "scan", "backup.add",
                "backup.list", "backup.restore", "log.record", "log.verify", "vault.info",
                "doctor"};
            bool ok = false;
            for (const char *name : allowed)
                ok = ok || op == name;
            if (!ok)
                throw SecurityError(ErrorCode::InvalidArgument, "不支持的 job op：" + op);
            std::string inner = body;
            const std::size_t pos = inner.find("\"job.submit\"");
            if (pos != std::string::npos)
                inner.replace(pos + 1, std::strlen("job.submit"), op);
            const std::string jobId = m_jobs->submit(op, [this, inner](std::string *jobError) {
                return dispatch(inner, jobError);
            });
            if (jobId.empty())
                throw SecurityError(ErrorCode::InvalidArgument, "任务队列已满（最多 64 个）");
            return okJson("{\"jobId\":\"" + jobId + "\",\"state\":\"queued\"}");
        }
        if (method == "job.status") {
            JobState state;
            if (!m_jobs->status(jsonField(body, "id"), &state))
                throw SecurityError(ErrorCode::InvalidArgument, "job 不存在");
            std::string json = "{\"id\":\"" + state.id + "\",\"op\":\"" +
                               util::jsonEscape(state.op) + "\",\"state\":\"" + state.state +
                               "\",\"progress\":" + std::to_string(state.progress);
            if (!state.result.empty())
                json += ",\"result\":" + state.result;
            if (!state.error.empty())
                json += ",\"error\":\"" + util::jsonEscape(state.error) + "\"";
            return okJson(json + "}");
        }
        if (method == "job.list")
            return okJson("{\"items\":" + m_jobs->listJson() + "}");
        if (method == "job.cancel") {
            if (!m_jobs->cancel(jsonField(body, "id")))
                throw SecurityError(ErrorCode::InvalidArgument, "job 不存在或无法取消");
            return okJson("{\"cancelled\":true}");
        }
        *error = "未知方法：" + method;
        return {};
    } catch (const SecurityError &ex) {
        *error = ex.what();
        return {};
    } catch (const std::exception &ex) {
        *error = ex.what();
        return {};
    }
}

} // namespace file_guard
