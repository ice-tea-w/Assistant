#include "security/SecurityCrypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace security::crypto {

namespace {

std::string pathUtf8(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

constexpr char kMagic[4] = {'F', 'G', 'C', '1'};

std::string statusText(NTSTATUS status)
{
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                  static_cast<unsigned long>(status));
    return buffer;
}

void checkNt(NTSTATUS status, const char *what)
{
    if (status < 0)
        throw SecurityError(ErrorCode::Platform,
                            std::string(what) + " 失败（NTSTATUS " + statusText(status) + "）");
}

struct AlgHandle {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~AlgHandle()
    {
        if (handle)
            BCryptCloseAlgorithmProvider(handle, 0);
    }
};

struct KeyHandle {
    BCRYPT_KEY_HANDLE handle = nullptr;
    ~KeyHandle()
    {
        if (handle)
            BCryptDestroyKey(handle);
    }
};

struct HashHandle {
    BCRYPT_HASH_HANDLE handle = nullptr;
    Bytes object;
    ~HashHandle()
    {
        if (handle)
            BCryptDestroyHash(handle);
    }
};

void putU32(Bytes &out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void putU64(Bytes &out, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t getU32(const std::uint8_t *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t getU64(const std::uint8_t *p)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | p[i];
    return value;
}

Bytes nonceFor(const Bytes &prefix, std::uint32_t index)
{
    Bytes nonce = prefix;
    putU32(nonce, index);
    return nonce;
}

Bytes aadFor(const Bytes &header, std::uint32_t index, bool final)
{
    Bytes aad = header;
    putU32(aad, index);
    aad.push_back(final ? 1 : 0);
    return aad;
}

AlgHandle openAesGcm()
{
    AlgHandle alg;
    checkNt(BCryptOpenAlgorithmProvider(&alg.handle, BCRYPT_AES_ALGORITHM, nullptr, 0),
            "BCryptOpenAlgorithmProvider(AES)");
    checkNt(BCryptSetProperty(alg.handle, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0),
            "BCryptSetProperty(GCM)");
    return alg;
}

KeyHandle makeAesKey(BCRYPT_ALG_HANDLE provider, const Bytes &key)
{
    KeyHandle handle;
    checkNt(BCryptGenerateSymmetricKey(provider, &handle.handle, nullptr, 0,
                                       const_cast<PUCHAR>(key.data()),
                                       static_cast<ULONG>(key.size()), 0),
            "BCryptGenerateSymmetricKey");
    return handle;
}

void gcmEncrypt(BCRYPT_KEY_HANDLE key, const Bytes &nonce, const Bytes &aad,
                const std::uint8_t *plain, std::size_t length, Bytes &out)
{
    out.resize(length + kTagSize);
    std::vector<std::uint8_t> tag(kTagSize, 0);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = const_cast<PUCHAR>(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());

    ULONG written = 0;
    checkNt(BCryptEncrypt(key, const_cast<PUCHAR>(plain), static_cast<ULONG>(length),
                          &info, nullptr, 0, out.data(), static_cast<ULONG>(length),
                          &written, 0),
            "BCryptEncrypt");
    if (written != length)
        throw SecurityError(ErrorCode::Platform, "AES-GCM 加密输出长度异常");
    std::memcpy(out.data() + length, tag.data(), kTagSize);
}

Bytes gcmDecrypt(BCRYPT_KEY_HANDLE key, const Bytes &nonce, const Bytes &aad,
                 const std::uint8_t *cipherWithTag, std::size_t length)
{
    if (length < kTagSize)
        throw SecurityError(ErrorCode::Corrupt, "密文分块长度不足");

    const std::size_t plainLength = length - kTagSize;
    Bytes plain(plainLength);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = const_cast<PUCHAR>(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = const_cast<PUCHAR>(cipherWithTag + plainLength);
    info.cbTag = static_cast<ULONG>(kTagSize);

    ULONG written = 0;
    const NTSTATUS status = BCryptDecrypt(
        key, const_cast<PUCHAR>(cipherWithTag), static_cast<ULONG>(plainLength),
        &info, nullptr, 0, plain.empty() ? nullptr : plain.data(),
        static_cast<ULONG>(plain.size()), &written, 0);
    if (status == static_cast<NTSTATUS>(0xC000A002)) // STATUS_AUTH_TAG_MISMATCH
        throw SecurityError(ErrorCode::Password, "GCM 校验失败：密码错误，或密文已损坏/被篡改");
    if (status < 0)
        throw SecurityError(ErrorCode::Password, "GCM 校验失败：密码错误，或密文已损坏/被篡改");
    if (written != plainLength)
        throw SecurityError(ErrorCode::Corrupt, "AES-GCM 解密输出长度异常");
    return plain;
}

class Hasher {
public:
    explicit Hasher(const Bytes &key = {})
    {
        const bool hmac = !key.empty();
        checkNt(BCryptOpenAlgorithmProvider(&m_alg.handle, BCRYPT_SHA256_ALGORITHM, nullptr,
                                           hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0),
                "BCryptOpenAlgorithmProvider(SHA256)");
        m_object.resize(1024);
        checkNt(BCryptCreateHash(m_alg.handle, &m_hash.handle, m_object.data(),
                                 static_cast<ULONG>(m_object.size()),
                                 hmac ? const_cast<PUCHAR>(key.data()) : nullptr,
                                 hmac ? static_cast<ULONG>(key.size()) : 0, 0),
                "BCryptCreateHash");
    }

    void update(const std::uint8_t *data, std::size_t length)
    {
        if (length == 0)
            return;
        checkNt(BCryptHashData(m_hash.handle, const_cast<PUCHAR>(data),
                               static_cast<ULONG>(length), 0),
                "BCryptHashData");
    }

    Bytes finishBytes()
    {
        Bytes digest(32);
        checkNt(BCryptFinishHash(m_hash.handle, digest.data(),
                                 static_cast<ULONG>(digest.size()), 0),
                "BCryptFinishHash");
        return digest;
    }

private:
    AlgHandle m_alg;
    HashHandle m_hash;
    Bytes m_object;
};

Bytes buildHeader(std::uint32_t iterations, std::uint32_t chunkSize,
                  const Bytes &salt, const Bytes &noncePrefix,
                  std::uint64_t plaintextSize)
{
    Bytes header;
    header.reserve(kHeaderSize);
    header.insert(header.end(), kMagic, kMagic + 4);
    header.push_back(1); // version
    header.push_back(1); // kdf_id = PBKDF2-HMAC-SHA256
    header.push_back(1); // cipher_id = AES-256-GCM
    header.push_back(0); // flags
    putU32(header, iterations);
    putU32(header, chunkSize);
    header.insert(header.end(), salt.begin(), salt.end());
    header.insert(header.end(), noncePrefix.begin(), noncePrefix.end());
    putU64(header, plaintextSize);
    const Bytes digest = sha256(header);
    header.insert(header.end(), digest.begin(), digest.end());
    return header;
}

ContainerInfo parseHeader(const Bytes &raw)
{
    if (raw.size() != kHeaderSize)
        throw SecurityError(ErrorCode::Corrupt, "文件过短，不是有效的加密容器");
    Bytes body(raw.begin(), raw.begin() + 48);
    const Bytes digest = sha256(body);
    if (!constantTimeEquals(Bytes(raw.begin() + 48, raw.end()), digest))
        throw SecurityError(ErrorCode::Corrupt, "容器头部校验失败（可能已损坏或被篡改）");
    if (std::memcmp(raw.data(), kMagic, 4) != 0)
        throw SecurityError(ErrorCode::Corrupt, "文件魔数不匹配，不是 FGC1 加密容器");
    const std::uint8_t version = raw[4];
    const std::uint8_t kdf = raw[5];
    const std::uint8_t cipher = raw[6];
    if (version != 1)
        throw SecurityError(ErrorCode::Corrupt, "不支持的容器版本");
    if (kdf != 1 || cipher != 1)
        throw SecurityError(ErrorCode::Corrupt, "不支持的算法组合");

    ContainerInfo info;
    info.iterations = getU32(raw.data() + 8);
    info.chunkSize = getU32(raw.data() + 12);
    info.salt.assign(raw.begin() + 16, raw.begin() + 32);
    info.noncePrefix.assign(raw.begin() + 32, raw.begin() + 40);
    info.plaintextSize = getU64(raw.data() + 40);
    if (info.iterations < kMinIterations || info.iterations > kMaxIterations)
        throw SecurityError(ErrorCode::Corrupt, "容器声明的 KDF 迭代次数异常");
    if (info.chunkSize < kMinChunkSize || info.chunkSize > kMaxChunkSize)
        throw SecurityError(ErrorCode::Corrupt, "容器声明的分块大小异常");
    return info;
}

crypto::Bytes readAllFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return crypto::Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::filesystem::path tempSibling(const std::filesystem::path &dst)
{
    return dst.parent_path() /
           (dst.filename().wstring() + L".part-" + std::to_wstring(::GetCurrentProcessId()) +
            L"-" + std::to_wstring(static_cast<unsigned long long>(::GetTickCount64())));
}

void replaceAtomically(const std::filesystem::path &tmp, const std::filesystem::path &dst)
{
    if (!MoveFileExW(tmp.wstring().c_str(), dst.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw SecurityError(ErrorCode::Io, "无法替换目标文件（可能被占用）");
}

} // namespace

// ---------------------------------------------------------------------------
// 原语
// ---------------------------------------------------------------------------
Bytes pbkdf2(const std::string &password, const Bytes &salt,
             std::uint32_t iterations, std::size_t length)
{
    if (iterations < kMinIterations || iterations > kMaxIterations)
        throw SecurityError(ErrorCode::InvalidArgument, "KDF 迭代次数超出允许范围");
    if (salt.size() < 16)
        throw SecurityError(ErrorCode::InvalidArgument, "盐长度不足 16 字节");
    if (password.empty())
        throw SecurityError(ErrorCode::Password, "密码不能为空");

    AlgHandle alg;
    checkNt(BCryptOpenAlgorithmProvider(&alg.handle, BCRYPT_SHA256_ALGORITHM, nullptr,
                                       BCRYPT_ALG_HANDLE_HMAC_FLAG),
            "BCryptOpenAlgorithmProvider(PBKDF2)");
    Bytes out(length);
    checkNt(BCryptDeriveKeyPBKDF2(alg.handle,
                                  reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())),
                                  static_cast<ULONG>(password.size()),
                                  const_cast<PUCHAR>(salt.data()),
                                  static_cast<ULONG>(salt.size()),
                                  static_cast<ULONGLONG>(iterations),
                                  out.data(), static_cast<ULONG>(out.size()), 0),
            "BCryptDeriveKeyPBKDF2");
    return out;
}

Bytes sha256(const Bytes &data)
{
    Hasher hasher;
    hasher.update(data.data(), data.size());
    return hasher.finishBytes();
}

std::string sha256Hex(const Bytes &data)
{
    static const char *digits = "0123456789abcdef";
    const Bytes digest = sha256(data);
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

Bytes hmacSha256(const Bytes &key, const Bytes &data)
{
    Hasher hasher(key);
    hasher.update(data.data(), data.size());
    return hasher.finishBytes();
}

Bytes randomBytes(std::size_t count)
{
    Bytes out(count);
    checkNt(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG),
            "BCryptGenRandom");
    return out;
}

Bytes aeadEncrypt(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &plain)
{
    AlgHandle aes = openAesGcm();
    KeyHandle aesKey = makeAesKey(aes.handle, key);
    Bytes out;
    gcmEncrypt(aesKey.handle, nonce, aad, plain.data(), plain.size(), out);
    return out;
}

Bytes aeadDecrypt(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &cipherWithTag)
{
    AlgHandle aes = openAesGcm();
    KeyHandle aesKey = makeAesKey(aes.handle, key);
    return gcmDecrypt(aesKey.handle, nonce, aad, cipherWithTag.data(), cipherWithTag.size());
}

bool constantTimeEquals(const Bytes &a, const Bytes &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

// ---------------------------------------------------------------------------
// 容器
// ---------------------------------------------------------------------------
ContainerInfo inspect(const std::filesystem::path &container)
{
    std::ifstream in(container, std::ios::binary);
    if (!in)
        throw SecurityError(ErrorCode::Io, "无法打开容器：" + pathUtf8(container));
    Bytes header(kHeaderSize);
    in.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    if (static_cast<std::size_t>(in.gcount()) != kHeaderSize)
        throw SecurityError(ErrorCode::Corrupt, "文件过短，不是有效的加密容器");
    return parseHeader(header);
}

Result encryptFile(const std::filesystem::path &src,
                   const std::filesystem::path &dst,
                   const std::string &password,
                   std::uint32_t iterations,
                   std::uint32_t chunkSize)
{
    if (iterations < kMinIterations || iterations > kMaxIterations)
        throw SecurityError(ErrorCode::InvalidArgument, "KDF 迭代次数超出允许范围");
    if (chunkSize < kMinChunkSize || chunkSize > kMaxChunkSize)
        throw SecurityError(ErrorCode::InvalidArgument, "分块大小超出允许范围");
    if (!std::filesystem::is_regular_file(src))
        throw SecurityError(ErrorCode::Io, "源文件不存在：" + pathUtf8(src));
    if (std::filesystem::exists(dst))
        throw SecurityError(ErrorCode::InvalidArgument, "目标文件已存在：" + pathUtf8(dst));

    const std::uint64_t total = std::filesystem::file_size(src);
    const std::uint32_t chunks = total == 0
                                     ? 0
                                     : static_cast<std::uint32_t>((total + chunkSize - 1) / chunkSize);

    Bytes salt = randomBytes(kSaltSize);
    Bytes noncePrefix = randomBytes(kNoncePrefixSize);
    const Bytes header = buildHeader(iterations, chunkSize, salt, noncePrefix, total);
    const Bytes key = pbkdf2(password, salt, iterations);

    AlgHandle aes = openAesGcm();
    KeyHandle aesKey = makeAesKey(aes.handle, key);

    const std::filesystem::path tmp = tempSibling(dst);
    Result result;
    result.plaintextSize = total;
    result.chunks = chunks;
    result.iterations = iterations;
    result.chunkSize = chunkSize;

    try {
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!in || !out)
            throw SecurityError(ErrorCode::Io, "打开文件失败");
        out.write(reinterpret_cast<const char *>(header.data()),
                  static_cast<std::streamsize>(header.size()));

        Bytes buffer(chunkSize);
        std::uint64_t done = 0;
        std::uint32_t index = 0;
        while (done < total) {
            const std::uint32_t want =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(chunkSize, total - done));
            in.read(reinterpret_cast<char *>(buffer.data()), want);
            if (static_cast<std::uint32_t>(in.gcount()) != want)
                throw SecurityError(ErrorCode::Io, "读取源文件失败（文件被修改？）");
            const bool final = (done + want == total);
            Bytes cipher;
            gcmEncrypt(aesKey.handle, nonceFor(noncePrefix, index),
                       aadFor(header, index, final), buffer.data(), want, cipher);
            out.write(reinterpret_cast<const char *>(cipher.data()),
                      static_cast<std::streamsize>(cipher.size()));
            done += want;
            ++index;
        }
        if (in.peek() != std::char_traits<char>::eof())
            throw SecurityError(ErrorCode::Io, "源文件在加密过程中被修改（变大），已中止");
        out.flush();
        if (!out)
            throw SecurityError(ErrorCode::Io, "写入容器失败（磁盘满？）");
        out.close();
        replaceAtomically(tmp, dst);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        throw;
    }
    result.sha256 = sha256(readAllFile(src));
    return result;
}

namespace {

Result decryptStream(const std::filesystem::path &src,
                     const std::filesystem::path *dst,
                     const std::string &password)
{
    std::ifstream in(src, std::ios::binary);
    if (!in)
        throw SecurityError(ErrorCode::Io, "无法打开容器：" + pathUtf8(src));

    Bytes header(kHeaderSize);
    in.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    if (static_cast<std::size_t>(in.gcount()) != kHeaderSize)
        throw SecurityError(ErrorCode::Corrupt, "文件过短，不是有效的加密容器");
    const ContainerInfo info = parseHeader(header);

    const Bytes key = pbkdf2(password, info.salt, info.iterations);
    AlgHandle aes = openAesGcm();
    KeyHandle aesKey = makeAesKey(aes.handle, key);

    const std::filesystem::path tmp = dst ? tempSibling(*dst) : std::filesystem::path();
    Result result;
    result.plaintextSize = info.plaintextSize;
    result.chunks = info.plaintextSize == 0
                        ? 0
                        : static_cast<std::uint32_t>((info.plaintextSize + info.chunkSize - 1) /
                                                     info.chunkSize);
    result.iterations = info.iterations;
    result.chunkSize = info.chunkSize;

    try {
        std::ofstream out;
        if (dst) {
            out.open(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                throw SecurityError(ErrorCode::Io, "无法创建临时输出文件");
        }
        Bytes buffer(static_cast<std::size_t>(info.chunkSize) + kTagSize);
        std::uint64_t done = 0;
        std::uint32_t index = 0;
        while (done < info.plaintextSize) {
            const std::uint32_t want = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(info.chunkSize, info.plaintextSize - done));
            in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(want) + kTagSize);
            if (static_cast<std::size_t>(in.gcount()) != want + kTagSize)
                throw SecurityError(ErrorCode::Corrupt, "容器被截断：密文分块长度不足");
            const bool final = (done + want == info.plaintextSize);
            const Bytes plain = gcmDecrypt(aesKey.handle, nonceFor(info.noncePrefix, index),
                                           aadFor(header, index, final), buffer.data(),
                                           want + kTagSize);
            if (dst)
                out.write(reinterpret_cast<const char *>(plain.data()),
                          static_cast<std::streamsize>(plain.size()));
            done += want;
            ++index;
        }
        if (in.peek() != std::char_traits<char>::eof())
            throw SecurityError(ErrorCode::Corrupt, "容器尾部存在多余数据");
        if (dst) {
            out.flush();
            if (!out)
                throw SecurityError(ErrorCode::Io, "写入明文失败（磁盘满？）");
            out.close();
            replaceAtomically(tmp, *dst);
        }
    } catch (...) {
        if (dst) {
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
        }
        throw;
    }
    return result;
}

} // namespace

Result decryptFile(const std::filesystem::path &src,
                   const std::filesystem::path &dst,
                   const std::string &password)
{
    if (std::filesystem::exists(dst))
        throw SecurityError(ErrorCode::InvalidArgument, "目标文件已存在：" + pathUtf8(dst));
    Result result = decryptStream(src, &dst, password);
    result.sha256 = sha256(readAllFile(dst));
    return result;
}

Result verifyFile(const std::filesystem::path &container, const std::string &password)
{
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() /
        ("fg-verify-" + std::to_string(::GetCurrentProcessId()) + "-" +
         std::to_string(::GetTickCount64()) + ".bin");
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    try {
        Result result = decryptStream(container, &tmp, password);
        result.sha256 = sha256(readAllFile(tmp));
        std::filesystem::remove(tmp, ignored);
        return result;
    } catch (...) {
        std::filesystem::remove(tmp, ignored);
        throw;
    }
}

Bytes encryptBytes(const Bytes &plain, const std::string &password,
                   std::uint32_t iterations, std::uint32_t chunkSize)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path src = dir / ("fg-src-" + std::to_string(::GetCurrentProcessId()) + ".bin");
    const std::filesystem::path dst = dir / ("fg-dst-" + std::to_string(::GetCurrentProcessId()) + ".fgc");
    std::error_code ignored;
    std::ofstream out(src, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(plain.data()),
              static_cast<std::streamsize>(plain.size()));
    out.close();
    try {
        encryptFile(src, dst, password, iterations, chunkSize);
        std::ifstream in(dst, std::ios::binary);
        Bytes result((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::filesystem::remove(src, ignored);
        std::filesystem::remove(dst, ignored);
        return result;
    } catch (...) {
        std::filesystem::remove(src, ignored);
        std::filesystem::remove(dst, ignored);
        throw;
    }
}

Bytes decryptBytes(const Bytes &container, const std::string &password)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path src = dir / ("fg-in-" + std::to_string(::GetCurrentProcessId()) + ".fgc");
    const std::filesystem::path dst = dir / ("fg-out-" + std::to_string(::GetCurrentProcessId()) + ".bin");
    std::error_code ignored;
    std::ofstream out(src, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(container.data()),
              static_cast<std::streamsize>(container.size()));
    out.close();
    try {
        decryptFile(src, dst, password);
        std::ifstream in(dst, std::ios::binary);
        Bytes result((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::filesystem::remove(src, ignored);
        std::filesystem::remove(dst, ignored);
        return result;
    } catch (...) {
        std::filesystem::remove(src, ignored);
        std::filesystem::remove(dst, ignored);
        throw;
    }
}

} // namespace security::crypto
