#ifndef ESPORTS_INPUT_STREAM_INPUT_H
#define ESPORTS_INPUT_STREAM_INPUT_H
#include "common/ffmpeg.h"
#include "common/logger.h"

namespace esports {
class StreamInput {
public:
    explicit StreamInput(IoControl& io) : io_(io) {}
    ~StreamInput();
    StreamInput(const StreamInput&) = delete;
    StreamInput& operator=(const StreamInput&) = delete;
    void open(const std::string& url, Logger& log);
    // 返回 FFmpeg 状态：成功、EOF 或 EAGAIN；其他错误抛异常。
    int read(AVPacket* packet);
    AVFormatContext* context() const { return context_; }
private:
    IoControl& io_;
    AVFormatContext* context_ = nullptr;
};
} // namespace esports
#endif
