#pragma once

#include <memory>
#include <mutex>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace SynapseX::Log {

inline spdlog::level::level_enum DefaultLevel() {
#if defined(NDEBUG)
    return spdlog::level::info;
#else
    return spdlog::level::debug;
#endif
}

inline std::shared_ptr<spdlog::logger> CreateLogger(
    const std::string& appName,
    spdlog::level::level_enum level,
    bool enableFileSink)
{
    std::vector<spdlog::sink_ptr> sinks;

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%n] %v");
    sinks.push_back(consoleSink);

    if (enableFileSink) {
        try {
            std::filesystem::create_directories("logs");
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (std::filesystem::path("logs") / (appName + ".log")).string(),
                5 * 1024 * 1024,
                3);
            fileSink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] [%n] [tid %t] %v");
            sinks.push_back(fileSink);
        } catch (const spdlog::spdlog_ex&) {
            // Fall back to console-only logging when file sink setup fails.
        } catch (const std::exception&) {
            // Fall back to console-only logging when file sink setup fails.
        }
    }

    auto logger = std::make_shared<spdlog::logger>(appName, sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);
    return logger;
}

inline void Initialize(
    const std::string& appName,
    spdlog::level::level_enum level = DefaultLevel(),
    bool enableFileSink = true)
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    static std::mutex initMutex;
    std::lock_guard<std::mutex> lock(initMutex);

    auto logger = CreateLogger(appName, level, enableFileSink);
    spdlog::set_default_logger(logger);
    spdlog::set_level(level);
}

inline void Shutdown() {
    spdlog::shutdown();
}

} // namespace SynapseX::Log

#define SX_LOG_IMPL(level, ...)                                                          \
    do {                                                                                 \
        if (auto* sx_logger__ = ::spdlog::default_logger_raw()) {                        \
            sx_logger__->log(::spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, \
                             level,                                                      \
                             __VA_ARGS__);                                               \
        }                                                                                \
    } while (0)

#define SX_LOG_TRACE(...) SX_LOG_IMPL(::spdlog::level::trace, __VA_ARGS__)
#define SX_LOG_DEBUG(...) SX_LOG_IMPL(::spdlog::level::debug, __VA_ARGS__)
#define SX_LOG_INFO(...) SX_LOG_IMPL(::spdlog::level::info, __VA_ARGS__)
#define SX_LOG_WARN(...) SX_LOG_IMPL(::spdlog::level::warn, __VA_ARGS__)
#define SX_LOG_ERROR(...) SX_LOG_IMPL(::spdlog::level::err, __VA_ARGS__)
#define SX_LOG_CRITICAL(...) SX_LOG_IMPL(::spdlog::level::critical, __VA_ARGS__)
