/*
 * 线程拥有者：构造即启动输入线程；析构先stop再join，避免线程访问已销毁的队列。
 * 主线程只能通过队列/原子统计交互，不能直接操作工作线程的解码器。
 */
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
    // stop发停止请求并关闭队列；join才保证线程已退出。先stop再join。
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
