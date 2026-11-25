// 安全模块 C++ 版验证程序（纯 C++，无 Qt/vcpkg）
//
// 用法：fg_tests <fixtureDir>（fixtureDir 内是 Python 版生成的 FGC1 容器）

#include "file_guard/audit.h"
#include "file_guard/backup.h"
#include "file_guard/config.h"
#include "file_guard/crypto.h"
#include "file_guard/integrity.h"
#include "file_guard/scan.h"
#include "file_guard/util.h"
#include "file_guard/vault.h"
#include "file_guard/watch.h"
#include "file_guard/zipx.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace file_guard;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string &what)
{
    ++g_checks;
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << "\n";
    if (!ok)
        ++g_failures;
}

std::string readText(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

crypto::Bytes readAll(const fs::path &path) { return util::readAll(path); }

std::string trim(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

void testCrypto(const fs::path &fixtures, const fs::path &work)
{
    std::cout << "\n[1] 加密容器（C++ 原生 FGC1）\n";
    (void)fixtures;
    const std::string password = "container-pass";
    const fs::path plain = work / "plain.bin";

    crypto::Bytes data;
    data.reserve(7624);
    for (int i = 0; i < 7624; ++i)
        data.push_back(static_cast<std::uint8_t>((i * 31 + 7) & 0xFF));
    util::writeAll(plain, data);
    const std::string expected = util::hexEncode(crypto::sha256(data));

    const fs::path container = work / "plain.bin.fgc";
    crypto::encryptFile(plain, container, password, 1000, 1024);
    const crypto::ContainerInfo info = crypto::inspect(container);
    check(info.iterations == 1000 && info.chunkSize == 1024, "头部参数写入正确");
    check(info.plaintextSize == data.size(), "明文长度写入正确");

    const crypto::Result verified = crypto::verifyFile(container, password);
    check(util::hexEncode(verified.sha256) == expected, "verify 明文哈希一致");

    const fs::path restored = work / "restored.bin";
    std::error_code ignored;
    fs::remove(restored, ignored);
    crypto::decryptFile(container, restored, password);
    check(util::readAll(restored) == data, "解密结果逐字节一致");

    bool wrongRejected = false;
    try {
        crypto::verifyFile(container, "wrong");
    } catch (const SecurityError &) {
        wrongRejected = true;
    }
    check(wrongRejected, "错误密码被拒绝");

    const fs::path tampered = work / "tampered.fgc";
    fs::copy_file(container, tampered, fs::copy_options::overwrite_existing);
    {
        std::fstream file(tampered, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end);
        char byte = 0;
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

    const fs::path brokenHeader = work / "broken-header.fgc";
    fs::copy_file(container, brokenHeader, fs::copy_options::overwrite_existing);
    {
        std::fstream file(brokenHeader, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(8, std::ios::beg);
        char byte = 0;
        file.seekg(8, std::ios::beg);
        file.read(&byte, 1);
        byte ^= 0x01;
        file.seekp(8, std::ios::beg);
        file.write(&byte, 1);
    }
    bool headerRejected = false;
    try {
        crypto::inspect(brokenHeader);
    } catch (const SecurityError &) {
        headerRejected = true;
    }
    check(headerRejected, "头部篡改被拒绝");
    fs::remove(brokenHeader, ignored);

    // 空文件
    const fs::path empty = work / "empty.bin";
    util::writeAll(empty, {});
    const fs::path emptyContainer = work / "empty.bin.fgc";
    crypto::encryptFile(empty, emptyContainer, password, 1000, 1024);
    const crypto::Result emptyVerified = crypto::verifyFile(emptyContainer, password);
    check(util::hexEncode(emptyVerified.sha256) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "空文件容器校验（SHA-256 空串向量）");

    // 大文件：加密/校验/解密，流式哈希与一次性哈希一致
    const fs::path large = work / "large.bin";
    crypto::Bytes largeData(3u << 20);
    for (std::size_t i = 0; i < largeData.size(); ++i)
        largeData[i] = static_cast<std::uint8_t>((i * 131 + 17) & 0xFF);
    util::writeAll(large, largeData);
    const fs::path largeContainer = work / "large.bin.fgc";
    crypto::encryptFile(large, largeContainer, password, 1000, 1u << 20);
    const crypto::Result largeVerified = crypto::verifyFile(largeContainer, password);
    check(util::hexEncode(largeVerified.sha256) == util::hexEncode(crypto::sha256File(large)),
          "大文件流式哈希与一次算完一致");
    const fs::path largeOut = work / "large.out.bin";
    fs::remove(largeOut, ignored);
    crypto::decryptFile(largeContainer, largeOut, password);
    check(util::readAll(largeOut) == largeData, "大文件解密逐字节一致");
}

void testVault(const fs::path &work)
{
    std::cout << "\n[2] 保险箱（DEK / 恢复码 / 名称加密 / 口令轮换）\n";
    const fs::path dir = work / "vault";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    Vault vault(dir, 1000, 1000);

    const auto created = vault.create("vault-pass");
    check(!created.recoveryCode.empty(), "创建并返回恢复码");
    check(fs::exists(dir / "vault.keys"), "密钥文件已生成");

    const fs::path source = work / util::fromUtf8("合同.docx");
    util::writeText(source, "保险箱测试内容");
    const VaultFileInfo added = vault.addFile(source, "note");
    check(added.container.rfind("containers/", 0) == 0, "容器名随机（containers/<hex>.fgc）");
    check(fs::exists(dir / added.container), "容器已落盘");

    bool plainNameLeaked = false;
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() == ".fgc")
            continue;
        if (readText(entry.path()).find("合同.docx") != std::string::npos)
            plainNameLeaked = true;
    }
    check(!plainNameLeaked, "原文件名不落盘（加密清单）");

    vault.lock();
    bool wrongRejected = false;
    try { vault.unlock("bad"); } catch (const SecurityError &) { wrongRejected = true; }
    check(wrongRejected, "错误口令被拒绝");
    vault.unlock("vault-pass");
    check(vault.list().size() == 1, "解锁后可见 1 个文件");

    const auto extracted = vault.extract("1", work / "out", true);
    check(extracted.name == "合同.docx", "取出还原原文件名");
    check(readText(work / "out" / util::fromUtf8("合同.docx")) == "保险箱测试内容", "取出内容正确");

    vault.changePassword("vault-pass", "vault-pass-2");
    vault.lock();
    bool oldRejected = false;
    try { vault.unlock("vault-pass"); } catch (const SecurityError &) { oldRejected = true; }
    check(oldRejected, "旧口令失效");
    vault.unlock("vault-pass-2");
    check(vault.verify().ok, "口令轮换后校验通过");

    Vault fresh(dir, 1000, 1000);
    fresh.unlockWithRecovery(created.recoveryCode);
    check(fresh.list().size() == 1, "恢复码可解锁");
    vault.unlock("vault-pass-2");
    check(vault.recoveryCode("vault-pass-2") == created.recoveryCode, "凭口令可重新展示恢复码");
}

void testBackup(const fs::path &work)
{
    std::cout << "\n[3] 备份（内容寻址 / 版本 / 冲突保护）\n";
    const fs::path dir = work / "backups";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    BackupStore store(dir, "backup-pass", 1000, 1000);

    const fs::path file = work / "report.txt";
    util::writeText(file, "version one");
    const BackupEntry first = store.backupFile(file);
    check(first.version == 1, "首次备份版本 1");
    check(store.versions(util::pathUtf8(fs::absolute(file))).size() == 1, "索引可见 1 个版本");

    util::writeText(file, "version two");
    const BackupEntry second = store.backupFile(file);
    check(second.version == 2, "再次备份版本 2");
    check(first.object != second.object, "内容变化 → 对象名变化（内容寻址）");

    fs::remove(file, ignored);
    const auto restored = store.restore(util::pathUtf8(fs::absolute(file)), 1, {}, false);
    check(restored.version == 1 && readText(file) == "version one", "按版本回滚（v1）");

    util::writeText(file, "unrelated change");
    bool conflictDetected = false;
    try { store.restore(util::pathUtf8(fs::absolute(file)), 1, {}, false); }
    catch (const SecurityError &) { conflictDetected = true; }
    check(conflictDetected, "未备份的改动拒绝覆盖（冲突保护）");

    const auto verify = store.verify(true);
    check(verify.ok && verify.objects == 2, "备份完整性校验（2 个对象）");
    check(store.gc() == 0, "gc 不删除被引用对象");
}

void testAuditScanIntegrity(const fs::path &work)
{
    std::cout << "\n[4] 审计 / 扫描 / 完整性\n";
    const fs::path auditDir = work / "audit";
    std::error_code ignored;
    fs::remove_all(auditDir, ignored);
    audit::AuditLog log(auditDir, "audit-pass", 1000);
    log.append("encrypt", "tester", "a.txt");
    log.append("delete", "tester", "b.txt");
    const auto verify = log.verify();
    check(verify.ok && verify.records == 2, "审计链校验通过");
    check(log.tail(1).size() == 1, "tail 可取最后一条");

    const fs::path logFile = auditDir / "audit.jsonl";
    std::string content = readText(logFile);
    const std::size_t pos = content.find("\"d\":\"");
    if (pos != std::string::npos) {
        const std::size_t target = pos + 5;
        content[target] = content[target] == 'A' ? 'B' : 'A';
        std::ofstream(logFile, std::ios::binary | std::ios::trunc) << content;
    }
    check(!log.verify().ok, "改一行 → 审计链校验失败");

    // 追加前一致性预检：删掉最后一行后，追加应被拒绝而不是写坏链
    const fs::path preflightDir = work / "audit-preflight";
    fs::remove_all(preflightDir, ignored);
    audit::AuditLog preflightLog(preflightDir, "preflight-pass", 1000);
    preflightLog.append("a", "t", "1");
    preflightLog.append("b", "t", "2");
    {
        const fs::path logFile = preflightDir / "audit.jsonl";
        std::string content = readText(logFile);
        const std::size_t lastNewline = content.find_last_of('\n');
        if (lastNewline != std::string::npos && lastNewline > 0) {
            const std::size_t prevNewline = content.find_last_of('\n', lastNewline - 1);
            content.resize(prevNewline == std::string::npos ? 0 : prevNewline + 1);
            std::ofstream(logFile, std::ios::binary | std::ios::trunc) << content;
        }
    }
    bool appendRejected = false;
    try {
        preflightLog.append("c", "t", "3");
    } catch (const SecurityError &) {
        appendRejected = true;
    }
    check(appendRejected, "尾部被截断后追加被拒绝（预检生效）");

    const auto findings = scan::scanText("身份证 11010519491231002X 卡号 4111111111111111", "sample.txt");
    check(findings.size() >= 2, "扫描识别身份证/银行卡");
    bool masked = true;
    for (const auto &finding : findings)
        masked = masked && finding.sample.find('*') != std::string::npos;
    check(masked, "样本打码");

    const fs::path manifest = work / "integrity.txt";
    const fs::path file = work / "module.txt";
    util::writeText(file, "module content");
    const auto built = buildIntegrity({file}, manifest, "integrity-pass");
    check(built.ok, "完整性清单生成");
    check(verifyIntegrity(manifest, "integrity-pass").ok, "完整性校验通过");
    util::writeText(file, "module content changed");
    check(!verifyIntegrity(manifest, "integrity-pass").ok, "文件被改 → 校验失败");
}


void testOfficeAndZip(const fs::path &fixtures)
{
    std::cout << "\n[5] Office 正文抽取（ZIP + DEFLATE）\n";
    const fs::path docx = fixtures / "contract.docx";
    check(fs::exists(docx), "docx fixture 存在");
    if (!fs::exists(docx))
        return;

    const auto entries = zipx::list(docx);
    check(entries.size() >= 3, "ZIP 目录可读");
    bool found = false;
    for (const auto &entry : entries) {
        if (entry.name == "word/document.xml") {
            const crypto::Bytes xml = zipx::readEntry(docx, entry);
            check(!xml.empty(), "document.xml 解压成功");
            found = true;
        }
    }
    check(found, "docx 含 word/document.xml");

    const std::string text = scan::readOfficeText(docx);
    check(!text.empty(), "Office 正文抽取非空");

    const scan::Report report = scan::scanPath(docx, false);
    check(report.filesScanned == 1, "docx 进入扫描（不是被当二进制跳过）");
}

void testBackupDiffPurge(const fs::path &work)
{
    std::cout << "\n[6] 备份 diff / purge\n";
    const fs::path dir = work / "backup-diff";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    BackupStore store(dir, "diff-pass", 1000, 1000);

    const fs::path file = work / "diff.txt";
    util::writeText(file, "alpha\nbeta\ngamma\n");
    store.backupFile(file);
    util::writeText(file, "alpha\nbeta changed\ngamma\ndelta\n");
    store.backupFile(file);
    const auto result = store.diff(util::pathUtf8(fs::absolute(file)));
    check(result.changed && result.kind == "text", "文本 diff 判定");
    check(result.text.find("-beta") != std::string::npos ||
              result.text.find("+beta changed") != std::string::npos,
          "unified diff 含变更行");

    for (int i = 0; i < 3; ++i) {
        util::writeText(file, "v" + std::to_string(i + 3));
        store.backupFile(file);
    }
    check(store.versions(util::pathUtf8(fs::absolute(file))).size() == 5, "索引 5 个版本");
    const std::size_t pruned = store.purge(2);
    check(pruned == 3, "purge 保留 2 个版本（清掉 3 个）");
    check(store.versions(util::pathUtf8(fs::absolute(file))).size() == 2, "purge 后索引剩 2");
}

void testWatchAutoEncrypt(const fs::path &work)
{
    std::cout << "\n[7] 目录监听 / 空闲自动加密\n";
    const fs::path dir = work / "watch";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir, ignored);
    const fs::path file = dir / "idle.txt";
    util::writeText(file, "idle content");
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
    std::filesystem::last_write_time(file, old, ignored);

    AutoEncryptOptions options;
    options.password = "watch-pass";
    options.idleSeconds = 60;
    options.iterations = 1000;
    options.chunkSize = 1024;
    const auto results = autoEncryptIdle({dir}, options);
    check(results.size() == 1 && results[0].action == "encrypted", "空闲文件被自动加密");
    check(!fs::exists(file), "明文已删除");
    check(fs::exists(fs::path(file.wstring() + L".fgc")), "容器已生成");
    check(crypto::verifyFile(fs::path(file.wstring() + L".fgc"), options.password).sha256.size() == 32,
          "自动加密结果可解密");
    check(collectFiles(dir, true).empty(), "collectFiles 跳过 .fgc");
}

void testDpapi()
{
    std::cout << "\n[8] DPAPI（当前用户绑定）\n";
    const crypto::Bytes secret = {'s', 'e', 'c', 'r', 'e', 't'};
    const crypto::Bytes sealed = util::dpapiProtect(secret);
    check(!sealed.empty(), "DPAPI 加密");
    check(util::dpapiUnprotect(sealed) == secret, "DPAPI 解密往返");
}

void testNewFeatures(const fs::path &fixtures, const fs::path &work)
{
    testOfficeAndZip(fixtures);
    testBackupDiffPurge(work);
    testWatchAutoEncrypt(work);
    testDpapi();
}

} // namespace

int main(int argc, char **argv)
{
    const fs::path fixtures = argc > 1 ? fs::path(argv[1]) : fs::path("tests/data");
    const fs::path work = fs::current_path() / "_fg_test_work";
    std::error_code ignored;
    fs::remove_all(work, ignored);
    fs::create_directories(work, ignored);

    std::cout << "file_guard C++ 验证\nfixture: " << util::pathUtf8(fixtures) << "\n";
    try {
        testCrypto(fixtures, work);
        testVault(work);
        testBackup(work);
        testAuditScanIntegrity(work);
        testNewFeatures(fixtures, work);
    } catch (const std::exception &error) {
        std::cerr << "[异常] " << error.what() << "\n";
        return 3;
    }

    std::cout << "\n结果：" << (g_checks - g_failures) << "/" << g_checks << " 通过";
    if (g_failures == 0) {
        std::cout << " ✔\n";
        return 0;
    }
    std::cout << "，失败 " << g_failures << " 项\n";
    return 1;
}
