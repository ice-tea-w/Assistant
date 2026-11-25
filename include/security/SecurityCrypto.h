#pragma once

// 安全模块 C++ 移植（M3）· 第 1 层：FGC1 加密容器
//
// 与 Python 原型 `tools/file_guard/crypto.py` **字节级兼容**：
//   * 80 字节大端头部（magic FGC1 / 版本 / KDF / cipher / 迭代数 / 分块大小 /
//     盐 / nonce 前缀 / 明文长度 / 头部 SHA-256）；
//   * 1 MiB 分块，每块 AES-256-GCM，nonce = prefix || BE32(块号)，
//     AAD = header || BE32(块号) || 1 字节"是否最后一块"；
//   * 解密先写临时文件，校验通过后原子替换，失败不留半截明文。
//
// 依赖：Windows CNG（bcrypt.dll）+ C++17 STL，不依赖 Qt/vcpkg。

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace security {

enum class ErrorCode : int {
    InvalidArgument = 1,
    Password = 3,
    Corrupt = 4,
    Integrity = 5,
    Io = 6,
    Platform = 7,
};

class SecurityError : public std::runtime_error {
public:
    SecurityError(ErrorCode code, const std::string &message)
        : std::runtime_error(message), m_code(code) {}
    ErrorCode code() const { return m_code; }

private:
    ErrorCode m_code;
};

namespace crypto {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kHeaderSize = 80;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kKeySize = 32;
constexpr std::size_t kSaltSize = 16;
constexpr std::size_t kNoncePrefixSize = 8;

constexpr std::uint32_t kDefaultIterations = 600'000;
constexpr std::uint32_t kMinIterations = 1'000;
constexpr std::uint32_t kMaxIterations = 5'000'000;

constexpr std::uint32_t kDefaultChunkSize = 1u << 20;
constexpr std::uint32_t kMinChunkSize = 256;
constexpr std::uint32_t kMaxChunkSize = 64u << 20;

struct ContainerInfo {
    std::uint32_t iterations = 0;
    std::uint32_t chunkSize = 0;
    std::uint64_t plaintextSize = 0;
    Bytes salt;
    Bytes noncePrefix;
};

struct Result {
    std::uint64_t plaintextSize = 0;
    std::uint32_t chunks = 0;
    std::uint32_t iterations = 0;
    std::uint32_t chunkSize = 0;
    Bytes sha256;   ///< 明文 SHA-256（32 字节）
};

// ---- 密码学原语（Windows CNG） ----
Bytes pbkdf2(const std::string &password, const Bytes &salt,
             std::uint32_t iterations, std::size_t length = kKeySize);
Bytes sha256(const Bytes &data);
std::string sha256Hex(const Bytes &data);
Bytes hmacSha256(const Bytes &key, const Bytes &data);
Bytes randomBytes(std::size_t count);

// 单块 AEAD（审计/密钥包裹复用；cipherWithTag 末尾 16 字节为 GCM tag）
Bytes aeadEncrypt(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &plain);
Bytes aeadDecrypt(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &cipherWithTag);
bool constantTimeEquals(const Bytes &a, const Bytes &b);

// ---- 容器 API ----
/// 只读头部（不需要密码），用于 info / 预检。
ContainerInfo inspect(const std::filesystem::path &container);

Result encryptFile(const std::filesystem::path &src,
                   const std::filesystem::path &dst,
                   const std::string &password,
                   std::uint32_t iterations = kDefaultIterations,
                   std::uint32_t chunkSize = kDefaultChunkSize);

Result decryptFile(const std::filesystem::path &src,
                   const std::filesystem::path &dst,
                   const std::string &password);

/// 解密到内存并返回明文哈希（不落盘）。
Result verifyFile(const std::filesystem::path &container,
                  const std::string &password);

/// 内存版本（测试/小数据用）。
Bytes encryptBytes(const Bytes &plain, const std::string &password,
                   std::uint32_t iterations = kDefaultIterations,
                   std::uint32_t chunkSize = kDefaultChunkSize);
Bytes decryptBytes(const Bytes &container, const std::string &password);

} // namespace crypto
} // namespace security
