/*
 * 单个输出会话的封装器；复制流描述后不依赖原描述对象存活。
 * finish用于报告写尾错误，析构只释放资源；调用者应显式完成finish。
 */
#ifndef ESPORTS_OUTPUT_STREAM_OUTPUT_H
#define ESPORTS_OUTPUT_STREAM_OUTPUT_H
#include "common/ffmpeg.h"
#include "common/logger.h"
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}

namespace esports {
class StreamOutput {
public:
    explicit StreamOutput(IoControl& io, bool low_latency = false, const std::string& format = "flv")
        : io_(io), low_latency_(low_latency), format_(format) {}
    ~StreamOutput();
    StreamOutput(const StreamOutput&) = delete;
    StreamOutput& operator=(const StreamOutput&) = delete;
    void open(const std::string& url, AVFormatContext* input, Logger& log);
    void open_encoded(const std::string& url, const std::vector<AVCodecContext*>& codecs, Logger& log);
    bool selected(int input_index) const;
    // packet 归调用者持有；写入会消耗其缓冲引用，失败也不能重用原包。
    void write(AVPacket* packet, AVRational input_time_base, int64_t origin_us);
    void finish();
private:
    static int write_file(void* opaque, uint8_t* buffer, int size);
    static int64_t seek_file(void* opaque, int64_t offset, int whence);
    void close() noexcept;
    IoControl& io_;
    bool low_latency_;
    std::string format_;
    AVFormatContext* context_ = nullptr;
    std::vector<int> mapping_;
    std::vector<int64_t> last_dts_;
    int fd_ = -1;
    bool custom_io_ = false;
    bool header_written_ = false;
    bool finished_ = false;
};
} // namespace esports
#endif
