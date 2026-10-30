#ifndef ESPORTS_INPUT_VIDEO_SOURCE_H
#define ESPORTS_INPUT_VIDEO_SOURCE_H
#include "queue/frame_queue.h"
#include "common/logger.h"
#include <atomic>
#include <thread>
namespace esports {
class VideoSource {
public:
    VideoSource(const std::string& url, int width, int height, int timeout_ms, Logger& log, bool common_origin = false, int offset_ms = 0, size_t queue_frames = 8);
    ~VideoSource();
    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;
    const CodecMetrics& metrics() const { return metrics_; }
    void stop();
    void join();
    FrameQueue& queue() { return queue_; }
    bool failed() const { return failed_.load(); }
private:
    void run(std::string url, int width, int height, int timeout_ms, Logger& log, bool common_origin, int offset_ms);
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    FrameQueue queue_;
    CodecMetrics metrics_;
    std::thread worker_;
};
}
#endif
