// 安全模块 C++ 移植的验证程序（纯 C++，不依赖 Qt/vcpkg）
//
// 覆盖：FGC1 容器（与 Python 原型互操作）/ 篡改检测 / 审计链 / 敏感扫描。
// 用法：assistant_security_tests <fixtureDir>（fixtureDir 内有 make_fixture.py 生成的数据）

#include "security/SecurityAudit.h"
#include "security/SecurityCrypto.h"
#include "security/SecurityScan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace security;

namespace {

std::string pathUtf8(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string &what)
{
    ++g_checks;
    if (ok) {
        std::cout << "  [PASS] " << what << "\n";
        return;
    }
    ++g_failures;
    std::cout << "  [FAIL] " << what << "\n";
}

std::string toHex(const crypto::Bytes &data)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::uint8_t byte : data) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

std::string trim(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

std::string readText(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

crypto::Bytes readBytes(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return crypto::Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void testContainerInterop(const fs::path &fixtures)
{
    std::cout << "\n[1] FGC1 容器：解密 Python 原型生成的容器\n";
    const fs::path container = fixtures / "python_made.fgc";
    const fs::path plain = fixtures / "plain.bin";
    const std::string expectedSha = trim(readText(fixtures / "plain.sha256"));
    const std::string password = "cpp-interop-pass";

    {
        const crypto::Bytes abc = {'a', 'b', 'c'};
        check(crypto::sha256Hex(abc) ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256 标准向量 abc");
        const crypto::Bytes plainBytes = readBytes(fixtures / "plain.bin");
        check(crypto::sha256Hex(plainBytes) == trim(readText(fixtures / "plain.sha256")),
              "一次算完 plain.bin 的 SHA-256");
    }
    check(fs::exists(container), "fixture 容器存在");
    if (!fs::exists(container))
        return;

    const crypto::ContainerInfo info = crypto::inspect(container);
    check(info.iterations == 1000, "头部迭代数 = 1000");
    check(info.chunkSize == 1024, "头部分块 = 1024（多块容器）");
    check(info.plaintextSize == fs::file_size(plain), "头部明文长度与源文件一致");
    check(info.salt.size() == 16 && info.noncePrefix.size() == 8, "盐/ nonce 前缀长度正确");

    const crypto::Result verify = crypto::verifyFile(container, password);
    check(toHex(verify.sha256) == expectedSha, "C++ 解出明文哈希 == Python 记录");
    check(verify.chunks > 1, "多块解密路径");

    const fs::path restored = fixtures / "cpp_restored.bin";
    std::error_code ignored;
    fs::remove(restored, ignored);
    crypto::decryptFile(container, restored, password);
    check(readBytes(restored) == readBytes(plain), "C++ 解密结果与源明文逐字节相同");

    bool wrongPasswordRejected = false;
    try {
        crypto::verifyFile(container, "wrong-password");
    } catch (const SecurityError &error) {
        wrongPasswordRejected = (error.code() == ErrorCode::Password ||
                                 error.code() == ErrorCode::Corrupt);
    }
    check(wrongPasswordRejected, "错误密码被拒绝");

    const fs::path tampered = fixtures / "tampered.fgc";
    fs::copy_file(container, tampered, fs::copy_options::overwrite_existing);
    {
        std::fstream file(tampered, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(-1, std::ios::end);
        char byte = 0;
        file.seekg(-1, std::ios::end);
        file.read(&byte, 1);
        byte ^= 0x5A;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    }
    bool tamperRejected = false;
    try {
        crypto::verifyFile(tampered, password);
    } catch (const SecurityError &) {
        tamperRejected = true;
    }
    check(tamperRejected, "篡改 1 字节被拒绝");
    fs::remove(tampered, ignored);
}

void testEncryptRoundTrip(const fs::path &fixtures)
{
    std::cout << "\n[2] C++ 加密 → 解密往返（并生成 cpp_made.fgc 供 Python 校验）\n";
    const fs::path plain = fixtures / "plain.bin";
    const fs::path made = fixtures / "cpp_made.fgc";
    const std::string password = "cpp-interop-pass";
    std::error_code ignored;
    fs::remove(made, ignored);

    crypto::encryptFile(plain, made, password, 1000, 1024);
    check(fs::exists(made), "C++ 生成容器");

    const crypto::Result verified = crypto::verifyFile(made, password);
    check(toHex(verified.sha256) == trim(readText(fixtures / "plain.sha256")),
          "C++ 容器解出的哈希 == 源明文哈希");

    const crypto::Bytes blob = readBytes(made);
    const crypto::Bytes roundTrip = crypto::decryptBytes(blob, password);
    check(roundTrip == readBytes(plain), "内存 API 往返一致");

    // 头部被改：必须在不需要密码时就被拒绝
    crypto::Bytes broken = blob;
    broken[8] ^= 0x01;
    bool headerRejected = false;
    try {
        crypto::decryptBytes(broken, password);
    } catch (const SecurityError &) {
        headerRejected = true;
    }
    check(headerRejected, "头部篡改被拒绝");
}

void testAudit(const fs::path &fixtures)
{
    std::cout << "\n[3] 审计日志：链校验 / 篡改 / 回滚\n";
    const fs::path dir = fixtures / "audit_tmp";
    std::error_code ignored;
    fs::remove_all(dir, ignored);

    audit::AuditLog log(dir, "audit-pass", 1000);
    log.append("encrypt", "tester", "合同.docx");
    log.append("delete", "tester", "旧备份");
    log.append("scan", "tester", "下载目录");
    const audit::VerifyResult ok = log.verify();
    check(ok.ok, "追加 3 条后校验通过");
    check(ok.records == 3, "记录数 = 3");
    check(!ok.head.empty(), "链头非空");

    const auto recent = log.tail(2);
    check(recent.size() == 2, "tail(2) 返回 2 条");
    check(!recent.empty() && recent.front().event == "delete", "tail 解密出正确事件");

    // 改第一行 → 校验失败
    const fs::path logFile = dir / "audit.jsonl";
    std::string content = readText(logFile);
    const std::size_t field = content.find("\"d\":\"");
    if (field != std::string::npos) {
        const std::size_t target = field + 5;
        content[target] = content[target] == 'A' ? 'B' : 'A';
        std::ofstream(logFile, std::ios::binary | std::ios::trunc) << content;
    }
    const audit::VerifyResult tampered = log.verify();
    check(!tampered.ok, "改一行 → 校验失败");
    check(!tampered.firstProblem.empty(), "给出失败原因");
}

void testScan(const fs::path &fixtures)
{
    std::cout << "\n[4] 敏感扫描：校验位 / 打码 / 目录扫描\n";
    const std::string text =
        "联系人：张三 11010519491231002X 电话 13800138000\n"
        "卡号 4111111111111111 合同编号 A-2026\n";
    const auto findings = scan::scanText(text, "合同.docx");
    bool hasId = false;
    bool hasBank = false;
    bool hasPhone = false;
    bool masked = true;
    for (const auto &finding : findings) {
        hasId = hasId || finding.rule == "id_card";
        hasBank = hasBank || finding.rule == "bank_card";
        hasPhone = hasPhone || finding.rule == "phone";
        if (finding.rule != "keyword.classified" && finding.rule != "filename.contract")
            masked = masked && finding.sample.find('*') != std::string::npos;
    }
    check(hasId, "识别有效身份证");
    check(hasBank, "识别有效银行卡（Luhn）");
    check(hasPhone, "识别手机号");
    check(masked, "样本一律打码");

    check(scan::isValidIdCard("11010519491231002X"), "身份证校验位正确");
    check(!scan::isValidIdCard("110105194912310021"), "身份证校验位错误被拒");
    check(scan::isValidLuhn("4111111111111111"), "Luhn 校验正确");
    check(!scan::isValidLuhn("4111111111111112"), "Luhn 校验错误被拒");

    std::ofstream(fixtures / "scan_sample.txt", std::ios::binary)
        << "身份证 11010519491231002X\n合同（保密）\n";
    const scan::Report report = scan::scanPath(fixtures / "scan_sample.txt");
    check(report.filesScanned == 1, "单文件扫描计数");
    check(report.high >= 1, "扫描到高危项");
    check(scan::maskValue("13800138000") == ("1" + std::string(9, '*') + "0"), "打码保留首尾");
}

} // namespace

int main(int argc, char **argv)
{
    const fs::path fixtures = argc > 1 ? fs::path(argv[1]) : fs::path("tests/security/data");
    if (!fs::exists(fixtures)) {
        std::cerr << "fixture 目录不存在：" << pathUtf8(fixtures) << "\n";
        return 2;
    }

    std::cout << "安全模块 C++ 移植验证\nfixture: " << pathUtf8(fixtures) << "\n" << std::flush;
    try {
        testContainerInterop(fixtures);
        testEncryptRoundTrip(fixtures);
        testAudit(fixtures);
        testScan(fixtures);
    } catch (const SecurityError &error) {
        std::cerr << "[exception] code=" << static_cast<int>(error.code())
                  << " what=" << error.what() << "\n";
        return 3;
    } catch (const std::exception &error) {
        std::cerr << "[std::exception] " << error.what() << "\n";
        return 4;
    }

    std::cout << "\n结果：" << (g_checks - g_failures) << "/" << g_checks << " 通过";
    if (g_failures == 0) {
        std::cout << " ✔\n";
        return 0;
    }
    std::cout << "，失败 " << g_failures << " 项\n";
    return 1;
}
