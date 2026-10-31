/*
 * 独立音频输入线程及其有界PCM队列；stop同时中断I/O并唤醒队列等待。
 */
#ifndef ESPORTS_INPUT_AUDIO_SOURCE_H
#define ESPORTS_INPUT_AUDIO_SOURCE_H
#include "queue/audio_queue.h"
#include "sync/audio_resampler.h"
#include "common/logger.h"
#include <atomic>
#include <thread>
namespace esports {
class AudioSource {
public:
    AudioSource(const std::string&, int timeout_ms, int offset_ms, Logger&, size_t queue_blocks = 32);
    ~AudioSource();
    const CodecMetrics& metrics() const { return metrics_; }
    // 只发出停止/唤醒请求，线程结束需再join；不能只join一个可能阻塞在满队列的线程。
    void stop();
    void join();
    AudioQueue& queue() { return queue_; }
private:
    void run(std::string, int timeout_ms, int offset_ms, Logger&);
    std::atomic<bool> stop_{false};
    AudioQueue queue_;
    CodecMetrics metrics_;
    std::thread worker_;
};
}
#endif
