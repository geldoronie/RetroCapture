#pragma once

#include <string>

class Logger {
public:
    static void init();
    static void shutdown();
    
    static void info(const std::string& message);
    static void error(const std::string& message);
    static void warn(const std::string& message);
    static void debug(const std::string& message);
    
private:
    static bool s_initialized;
};

// Macros de conveniência
#define LOG_INFO(msg) Logger::info(msg)
#define LOG_ERROR(msg) Logger::error(msg)
#define LOG_WARN(msg) Logger::warn(msg)
#define LOG_DEBUG(msg) Logger::debug(msg)

