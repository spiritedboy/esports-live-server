/*
 * 初学者导读
 * 每个实例代表一路视频，构造函数启动 worker；run 中的 FFmpeg 对象只由 worker 使用。
 * width/height 是交给合成器的统一尺寸，timeout_ms 是输入 I/O 期限。
 * offset_ms 是媒体时间偏移：正数让该源画面在节目中晚出现，而非延迟网络连接。
 * queue_frames 限制缓存帧数量；主线程通过 queue().advance 选帧，不直接读解码器。
 * metrics 用原子计数支持跨线程观察；failed 表示该路异常，不代表整个节目必须退出。
 * stop 发出取消并唤醒队列，join 等待退出；析构自动依次执行这两步。
 * Logger 由外部持有，必须覆盖整个线程生命期；成员 worker 最后声明以先初始化其依赖。
 */
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
    VideoSource(const std::string& url, int width, int height, int timeout_ms, Logger& log, int offset_ms = 0, size_t queue_frames = 4);
    ~VideoSource();
    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;
    // 只读访问解码统计，内部计数是原子的；返回引用不能超过本源对象的生命期。
    const CodecMetrics& metrics() const { return metrics_; }
    // stop发停止请求并关闭队列；join才保证线程已退出。先stop再join。
    void stop();
    // 等待 worker 真正结束；单独调用不发停止请求，应先 stop 或确定输入已经自然结束。
    void join();
    FrameQueue& queue() { return queue_; }
    bool failed() const { return failed_.load(); }
private:
    void run(std::string url, int width, int height, int timeout_ms, Logger& log, int offset_ms);
    // 主线程写、工作线程和 I/O 中断回调读的取消标志，避免普通 bool 的数据竞争。
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    FrameQueue queue_;
    CodecMetrics metrics_;
    // 持有线程句柄；不能让仍可 join 的 std::thread 直接析构，否则会 terminate。
    std::thread worker_;
};
}
#endif
