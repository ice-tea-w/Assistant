#pragma once

// 本地 HTTP JSON-RPC 服务（回环 + token + Host/Origin 校验）
//
// 与 Python 版 service.py 语义一致：默认只绑 127.0.0.1；请求必须带 X-FG-Token；
// 带 Origin 的请求一律拒绝（防浏览器 CSRF）；Host 只接受回环名（防 DNS rebinding）。

#include "file_guard/config.h"

#include <memory>
#include <string>

namespace file_guard {

struct ServiceOptions {
    int port = 8765;
    bool allowRemote = false;
    bool once = false;      ///< 处理一个请求后退出（测试用）
};

class JobManager;

class LocalService {
public:
    LocalService(Config config, ServiceOptions options);
    ~LocalService();
    int run();

    /// 确保 token 文件存在，返回 token（十六进制）。
    static std::string ensureToken(const std::filesystem::path &tokenFile);

private:
    std::string dispatch(const std::string &body, std::string *error);
    std::string handleConnection(unsigned long long socket, std::string *error);

    Config m_config;
    ServiceOptions m_options;
    std::string m_token;
    std::unique_ptr<JobManager> m_jobs;
};

} // namespace file_guard
