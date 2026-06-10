#pragma once

#include <string>

class Logger {
public:
    enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

    static void init();
    static void shutdown();

    static void info(const std::string& message);
    static void error(const std::string& message);
    static void warn(const std::string& message);
    static void debug(const std::string& message);

    // Minimum level actually emitted. Defaults to Info, so per-frame Debug
    // diagnostics stay out of the console/log file unless explicitly asked
    // for (env RETROCAPTURE_LOG_LEVEL=debug, parsed in init()).
    static void setLevel(Level level);
    static Level getLevel();

private:
    static bool s_initialized;
    static Level s_level;
};

// Macros de conveniência
#define LOG_INFO(msg) Logger::info(msg)
#define LOG_ERROR(msg) Logger::error(msg)
#define LOG_WARN(msg) Logger::warn(msg)
#define LOG_DEBUG(msg) Logger::debug(msg)
