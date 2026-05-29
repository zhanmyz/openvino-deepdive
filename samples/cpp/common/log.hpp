#pragma once
// ============================================================
// Thread-safe logging with levels and timestamps (header-only)
// ============================================================
//
// Usage:
//   LOG(INFO)  << "Server started on port " << port << std::endl;
//   LOG(DEBUG) << "Thread #" << id << " acquired lock" << std::endl;
//   LOG(WARN)  << "Queue depth exceeds threshold" << std::endl;
//   LOG(ERROR) << "Failed to open file: " << path << std::endl;
//
// Output format:
//   [HH:MM:SS.mmm] [INFO ] Server started on port 8080
//   [HH:MM:SS.mmm] [DEBUG] Thread #3 acquired lock
//
// Compile-time level filter (set via CMake or -D):
//   -DLOG_MIN_LEVEL=0  → show all (DEBUG and above)  [default in Debug]
//   -DLOG_MIN_LEVEL=1  → INFO and above              [default in Release]
//   -DLOG_MIN_LEVEL=2  → WARN and above
//   -DLOG_MIN_LEVEL=3  → ERROR only
//
// Thread safety:
//   Each LOG() creates a temporary LogMessage object on the stack.
//   The message is buffered in an ostringstream, then flushed atomically
//   (under a mutex) in the destructor. No interleaving between threads.

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

// Log severity levels
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

#ifndef LOG_MIN_LEVEL
#define LOG_MIN_LEVEL 0
#endif

namespace detail {

inline std::mutex& log_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline const char* log_level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?????";
}

}  // namespace detail

struct LogMessage {
    std::ostringstream oss;
    LogLevel level;

    explicit LogMessage(LogLevel lvl) : level(lvl) {}
    LogMessage(const LogMessage&) = delete;
    LogMessage& operator=(const LogMessage&) = delete;

    template<typename T>
    LogMessage& operator<<(const T& val) { oss << val; return *this; }

    // Support std::endl, std::flush, etc.
    LogMessage& operator<<(std::ostream& (*manip)(std::ostream&)) {
        manip(oss); return *this;
    }

    // Support std::fixed, std::hex, etc.
    LogMessage& operator<<(std::ios_base& (*manip)(std::ios_base&)) {
        manip(oss); return *this;
    }

    ~LogMessage() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        std::tm tm_buf;
        localtime_r(&time_t_now, &tm_buf);

        std::lock_guard<std::mutex> lock(detail::log_mutex());
        std::cerr << "[" << std::put_time(&tm_buf, "%H:%M:%S")
                  << "." << std::setfill('0') << std::setw(3) << ms.count()
                  << "] [" << detail::log_level_tag(level) << "] "
                  << oss.str();
    }
};

// Primary logging macro — filtered at compile time by LOG_MIN_LEVEL.
// When the level is below the threshold, the entire stream expression
// is optimized away (dead code elimination).
#define LOG(level) \
    if (static_cast<int>(LogLevel::level) < LOG_MIN_LEVEL) {} \
    else LogMessage(LogLevel::level)
