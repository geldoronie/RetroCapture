#include "Logger.h"
#include "Paths.h"
#include <iostream>
#include <fstream>
#include <mutex>

bool Logger::s_initialized = false;

namespace
{
// Mirror every log line to a file as well as the console, so GUI / .app
// launches (which have no terminal — e.g. macOS bundles needed for the
// Screen-Recording permission) are still debuggable. Truncated each run.
std::ofstream g_logFile;
std::mutex    g_logMutex;

void writeLine(std::ostream &console, const char *level, const std::string &message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    console << level << message << std::endl;
    if (g_logFile.is_open())
    {
        g_logFile << level << message << "\n";
        g_logFile.flush();
    }
}
} // namespace

void Logger::init()
{
    if (s_initialized)
    {
        return;
    }
    s_initialized = true;

    // getUserDataDir() creates the directory (ensureDir), so the file can
    // be opened directly. Failure is non-fatal — console logging still works.
    std::string logPath;
    try
    {
        logPath = Paths::getUserDataDir() + "/retrocapture.log";
        g_logFile.open(logPath, std::ios::out | std::ios::trunc);
    }
    catch (...)
    {
    }

    info("Logger initialized");
    if (g_logFile.is_open())
    {
        info("Log file: " + logPath);
    }
}

void Logger::shutdown()
{
    if (!s_initialized)
    {
        return;
    }
    s_initialized = false;
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open())
    {
        g_logFile.close();
    }
}

void Logger::info(const std::string &message)
{
    writeLine(std::cout, "[INFO] ", message);
}

void Logger::error(const std::string &message)
{
    writeLine(std::cerr, "[ERROR] ", message);
}

void Logger::warn(const std::string &message)
{
    writeLine(std::cout, "[WARN] ", message);
}

void Logger::debug(const std::string &message)
{
    writeLine(std::cout, "[DEBUG] ", message);
}
