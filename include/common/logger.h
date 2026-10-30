#ifndef ESPORTS_COMMON_LOGGER_H
#define ESPORTS_COMMON_LOGGER_H

#include <mutex>
#include <ostream>
#include <string>

namespace esports {
enum class LogLevel { Debug, Info, Warning, Error };

// 输出流由调用者持有，必须比 Logger 及所有写入线程活得更久。
// 同一输出流应共用一个 Logger；输出包含 UTC 时间和级别。
class Logger {
public:
    explicit Logger(std::ostream& output, LogLevel minimum = LogLevel::Info);
    void log(LogLevel level, const std::string& message);
private:
    std::ostream& output_;
    const LogLevel minimum_;
    std::mutex mutex_;
};
} // namespace esports
#endif
