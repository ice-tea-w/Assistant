#pragma once

// 安全模块 C++ 移植（M3）· 第 2 层：敏感文件检测
//
// 与 Python 版 scan.py 同语义的规则子集：身份证（mod 11）、银行卡（Luhn）、
// 统一社会信用代码（mod 31）、手机号、敏感关键词。只读文本、不联网，
// 报告里的样本一律打码。

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace file_guard::scan {

enum class Severity { Info = 0, Low = 1, Medium = 2, High = 3 };

struct Finding {
    std::string rule;
    Severity severity = Severity::Low;
    std::string sample;   ///< 已打码
    std::size_t line = 0;
};

struct FileResult {
    std::filesystem::path path;
    std::size_t bytes = 0;
    std::vector<Finding> findings;
    bool skipped = false;
    std::string reason;
};

struct Report {
    std::size_t filesScanned = 0;
    std::size_t filesSkipped = 0;
    std::size_t high = 0;
    std::size_t medium = 0;
    std::size_t low = 0;
    std::vector<FileResult> results;
};

/// 扫描单个文件或目录（目录默认递归；跳过重解析点/系统目录/大文件）。
Report scanPath(const std::filesystem::path &path, bool recursive = true);

/// 从 docx/xlsx/pptx 的 zip 包中抽取 XML 正文（无第三方依赖；防 zip 炸弹）。
std::string readOfficeText(const std::filesystem::path &path);

/// 只扫描已读入的文本（测试/内存用）。
std::vector<Finding> scanText(const std::string &text, const std::string &filename);

// ---- 校验位（导出供测试） ----
bool isValidIdCard(const std::string &digits);
bool isValidLuhn(const std::string &digits);
bool isValidUscc(const std::string &code);
std::string maskValue(const std::string &value);

} // namespace file_guard::scan
