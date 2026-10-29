#include "core/Logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/common.h>

#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

static std::shared_ptr<spdlog::logger> g_logger;

void Logger::init(const std::string &logDir, const std::string &level)
{
#ifdef _WIN32
    // Windows 控制台默认使用 GBK(936)，UTF-8 日志字节会被按 GBK 误解码成乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // Build sinks
    std::vector<spdlog::sink_ptr> sinks;

    // Console sink — colored output for readability during development
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    sinks.push_back(std::move(consoleSink));

    // File sink — rotating: 5 MB per file, keep 3 rotated files
    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        logDir + "/assistant.log", 5 * 1024 * 1024, 3);
    fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
    sinks.push_back(std::move(fileSink));

    // Create logger
    g_logger = std::make_shared<spdlog::logger>("assistant", std::begin(sinks), std::end(sinks));
    g_logger->set_level(spdlog::level::from_str(level));
    g_logger->flush_on(spdlog::level::info);

    // Register globally so spdlog::get("assistant") works
    spdlog::register_logger(g_logger);
}

spdlog::logger &Logger::get()
{
    if (!g_logger) {
        // Fallback: create a console-only logger with default settings.
        // This prevents crashes if someone calls Logger::get() before init().
        g_logger = spdlog::stdout_color_mt("assistant_fallback");
        g_logger->warn("Logger::get() called before init() — using console-only fallback");
    }
    return *g_logger;
}

void Logger::setLevel(const std::string &level)
{
    if (g_logger) {
        g_logger->set_level(spdlog::level::from_str(level));
        g_logger->info("Log level changed to: {}", level);
    }
}
