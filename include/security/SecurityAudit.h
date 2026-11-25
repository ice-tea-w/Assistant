#pragma once

// 安全模块 C++ 移植（M3）· 第 3 层：审计日志
//
// 语义与 Python 版 audit.py v2 一致（逐条随机 nonce + HMAC 链 + meta 高水位），
// 但文件格式是 **C++ 版 v2**（`audit.meta.json` + `audit.jsonl`，字段名相同）。
// Python 版日志的迁移工具列入下一阶段（两版不共享密钥派生参数，需显式迁移）。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace security::audit {

struct Record {
    std::uint64_t seq = 0;
    std::string event;
    std::string actor;
    std::string detail;
    std::string timestamp;
};

struct VerifyResult {
    bool ok = false;
    std::uint64_t records = 0;
    std::size_t problems = 0;
    std::string head;
    std::string firstProblem;
};

class AuditLog {
public:
    /// @param iterations 审计日志的 KDF 迭代数（测试可传 1000；生产建议 ≥ 200000）
    AuditLog(std::filesystem::path directory, std::string password,
             std::uint32_t iterations = 100'000);

    /// 追加一条记录，返回其序号（0 起）。
    std::uint64_t append(const std::string &event, const std::string &actor,
                         const std::string &detail);

    /// 全量校验：meta 签名 + 每行链哈希 + 序号连续 + 高水位（防回滚/截断）。
    VerifyResult verify() const;

    /// 取最后 limit 条（解密后）。
    std::vector<Record> tail(std::size_t limit) const;

    std::string head() const;
    std::uint64_t nextSeq() const;

    const std::filesystem::path &directory() const { return m_directory; }

private:
    std::filesystem::path m_directory;
    std::string m_password;
    std::uint32_t m_iterations = 100'000;
};

} // namespace security::audit
