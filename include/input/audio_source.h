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
