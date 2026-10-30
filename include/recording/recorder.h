#ifndef ESPORTS_RECORDING_RECORDER_H
#define ESPORTS_RECORDING_RECORDER_H
#include "output/stream_output.h"
#include "queue/packet_queue.h"
#include <thread>
namespace esports {
struct RecordingOptions {
    std::string prefix, format = "mp4";
    int segment_ms = 60000;
    size_t queue_bytes = 16 * 1024 * 1024;
};
// 单生产者；失败只关闭录制支路。每个输出拥有独立的包引用。
class Recorder {
public:
    Recorder(const RecordingOptions&, Logger&);
    ~Recorder();
    void open(const std::vector<AVCodecContext*>&);
    void submit(const AVPacket*, AVRational);
    void finish();
private:
    void run();
    void fail(const std::string&);
    RecordingOptions options_;
    Logger& log_;
    AVFormatContext* description_ = nullptr;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedPacket> queue_;
    size_t bytes_ = 0;
    bool closed_ = false, failed_ = false;
};
}
#endif
