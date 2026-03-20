#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>

namespace orch {

// ─────────────────────────────────────────────
// UUID Generation
// ─────────────────────────────────────────────

inline std::string generate_uuid() {
    static thread_local std::mt19937 rng(
        std::random_device{}()
    );
    static thread_local std::uniform_int_distribution<int> dist(0, 15);

    const char* hex = "0123456789abcdef";
    // Format: 8-4-4-4-12
    const int groups[] = {8, 4, 4, 4, 12};
    std::string uuid;
    uuid.reserve(36);

    for (int g = 0; g < 5; ++g) {
        if (g > 0) uuid += '-';
        for (int i = 0; i < groups[g]; ++i) {
            uuid += hex[dist(rng)];
        }
    }
    return uuid;
}

// ─────────────────────────────────────────────
// Timestamps
// ─────────────────────────────────────────────

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

inline std::string timestamp_str() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count() % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms;
    return oss.str();
}

// ─────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────

enum class LogLevel { INFO, WARN, ERROR };

inline const char* level_str(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?????";
}

inline void log(LogLevel level, const std::string& component,
                const std::string& message) {
    std::cout << "[" << timestamp_str() << "] "
              << level_str(level) << " "
              << "[" << component << "] "
              << message << std::endl;
}

inline void log_info(const std::string& component, const std::string& msg) {
    log(LogLevel::INFO, component, msg);
}

inline void log_warn(const std::string& component, const std::string& msg) {
    log(LogLevel::WARN, component, msg);
}

inline void log_error(const std::string& component, const std::string& msg) {
    log(LogLevel::ERROR, component, msg);
}

}  // namespace orch
