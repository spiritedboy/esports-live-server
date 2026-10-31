/*
 * 异步推流边界：主线程open/submit/check/finish，工作线程独占PacketSink。
 * 工作线程异常保存在exception_ptr，由主线程check/finish重新抛出。
 */
#ifndef ESPORTS_OUTPUT_ASYNC_OUTPUT_H
#define ESPORTS_OUTPUT_ASYNC_OUTPUT_H
#include "queue/packet_queue.h"
#include "output/stream_output.h"
#include <functional>
#include <thread>
namespace esports {
struct OutputOptions {
    PacketQueueLimits queue;
    int timeout_ms = 3000;
    int reconnect_attempts = 5;
    int backoff_ms = 250;
};
struct OutputStats {
    uint64_t video_packets = 0, audio_packets = 0, encoded_bytes = 0;
    uint64_t attempts = 0, sessions = 0, failures = 0, retries = 0;
    int64_t elapsed_us = 0, max_queue_us = 0, max_write_us = 0;
    PacketQueueStats queue;
};
// 可替换边界仅用于故障注入测试；生产默认始终使用 FFmpeg 实际输出。
class PacketSink {
public:
    virtual ~PacketSink() {}
    virtual void open(const std::string&, AVFormatContext*, Logger&) = 0;
    virtual void write(AVPacket*, AVRational, int64_t origin_us) = 0;
    virtual void finish() = 0;
};
typedef std::function<std::unique_ptr<PacketSink>(IoControl&)> SinkFactory;
class AsyncOutput {
public:
    AsyncOutput(const OutputOptions&, bool network, Logger&, SinkFactory factory = SinkFactory());
    ~AsyncOutput();
    AsyncOutput(const AsyncOutput&) = delete;
    AsyncOutput& operator=(const AsyncOutput&) = delete;
    void open(const std::string&, const std::vector<AVCodecContext*>&);
    // 接管Packet；调用后原unique_ptr为空。网络可丢过期数据，本地队列满会背压等待。
    void submit(Packet, AVRational);
    bool take_keyframe_request() { return queue_.take_key_request(); }
    void reset_for_latency() { if (network_) queue_.reset(); }
    void check() const;
    void finish(const volatile std::sig_atomic_t* user_stop = nullptr);
    void cancel();
    OutputStats snapshot() const;
private:
    void run(std::string url);
    OutputOptions options_;
    bool network_;
    Logger& log_;
    SinkFactory factory_;
    PacketQueue queue_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    AVFormatContext* description_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::exception_ptr error_;
    OutputStats stats_;
    int64_t started_us_ = 0, ended_us_ = 0;
    bool finished_ = false;
};
}
#endif
