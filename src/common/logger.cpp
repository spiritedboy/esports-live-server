/*
 * 多个媒体线程共享日志器；一条消息在同一互斥锁内完成格式化和输出。
 * 日志用UTC系统时间便于定位事件，媒体调度则使用单调时钟，二者用途不同。
 */
#include "common/logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace esports {
namespace {
const char* level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}
}

Logger::Logger(std::ostream& output, LogLevel minimum)
    : output_(output), minimum_(minimum) {}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < minimum_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc = {};
    gmtime_r(&now, &utc);
    std::ostringstream line;
    line << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ")
         << " [" << level_name(level) << "] ";
    // 防止消息换行伪造新的日志记录。
    for (char c : message) {
        if (c == '\n') line << "\\n";
        else if (c == '\r') line << "\\r";
        else line << c;
    }
    output_ << line.str() << '\n';
    output_.flush();
}
} // namespace esports
