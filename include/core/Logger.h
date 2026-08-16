#pragma once

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

/// @brief Cross-cutting logging singleton wrapping spdlog.
///
/// Usage:
///   Logger::init("E:/Assistant/logs", "debug");
///   Logger::get().info("AppBridge: search query '{}'", query.toStdString());
///
/// Not a QObject — logging is infrastructure, not a QML-facing API.
class Logger
{
public:
    /// Initialize the singleton. Call once at startup, before any log output.
    /// @param logDir   Absolute path to the log directory.
    /// @param level    "trace" | "debug" | "info" | "warn" | "error" | "critical".
    static void init(const std::string &logDir, const std::string &level = "info");

    /// Access the underlying spdlog logger (e.g. for LOG_XXX macros).
    static spdlog::logger &get();

    /// Re-configure log level at runtime (e.g. when user changes settings).
    static void setLevel(const std::string &level);

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

private:
    Logger() = default;
};
