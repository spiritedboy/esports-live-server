/*
 * 初学者导读
 * 音频输入独立于视频输入；即使 URL 相同，也会独立打开一次解复用连接。
 * 工作线程输出统一的 48kHz、双声道、平面浮点音频，主线程从 AudioQueue 取采样窗口。
 * 构造参数依次为 URL、I/O 超时毫秒、媒体偏移毫秒、借用的日志器、队列块数。
 * 1ms 对应 48 个采样位置；这里的采样位置按每声道计，不因双声道而乘二。
 * 没有音轨或该路失败时关闭队列，上层用静音填充；本类不实现输入自动重连。
 * stop_ 是跨线程取消标志；queue_ 负责数据交接；worker_ 在析构前必须 join。
 * metrics() 返回内部对象的只读引用，不能在 AudioSource 销毁后继续持有。
 */
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
    // 只读访问解码统计，内部计数是原子的；返回引用不能超过本源对象的生命期。
    const CodecMetrics& metrics() const { return metrics_; }
    // 只发出停止/唤醒请求，线程结束需再join；不能只join一个可能阻塞在满队列的线程。
    void stop();
    // 等待 worker 真正结束；单独调用不发停止请求，应先 stop 或确定输入已经自然结束。
    void join();
    AudioQueue& queue() { return queue_; }
private:
    void run(std::string, int timeout_ms, int offset_ms, Logger&);
    // 主线程写、工作线程和 I/O 中断回调读的取消标志，避免普通 bool 的数据竞争。
    std::atomic<bool> stop_{false};
    AudioQueue queue_;
    CodecMetrics metrics_;
    // 持有线程句柄；不能让仍可 join 的 std::thread 直接析构，否则会 terminate。
    std::thread worker_;
};
}
#endif
