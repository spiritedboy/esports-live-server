/*
 * 初学者导读
 * 调用关系：各输入线程、节目线程和输出线程 → 同一个 Logger → 指定输出流。
 * Logger 借用 ostream，不负责创建或销毁它；调用者必须保证输出流更晚销毁。
 * minimum 是过滤阈值；比阈值低的消息直接忽略。等级顺序依赖下面枚举的顺序。
 * mutex 保护完整的一条日志，避免线程 A 的半行与线程 B 的半行拼在一起。
 * 这个类只负责诊断信息；其 UTC 时间不能用作音视频 PTS 或超时计时器。
 */
/*
 * 线程安全日志接口；借用的输出流须比Logger及所有调用线程活得更久。
 */
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
