#include "Logger.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>

bool Logger::s_initialized = false;

void Logger::init() {
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    info("Logger inicializado");
}

void Logger::shutdown() {
    if (!s_initialized) {
        return;
    }
    s_initialized = false;
}

void Logger::info(const std::string& message) {
    std::cout << "[INFO] " << message << std::endl;
}

void Logger::error(const std::string& message) {
    std::cerr << "[ERROR] " << message << std::endl;
}

void Logger::warn(const std::string& message) {
    std::cout << "[WARN] " << message << std::endl;
}

void Logger::debug(const std::string& message) {
    std::cout << "[DEBUG] " << message << std::endl;
}

