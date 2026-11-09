/*
 * 初学者导读
 * 视频可以重复上一帧，音频则必须按连续采样区间拼接，不能重复上一块声音。
 * 生产者送入按 PTS 排序、不重叠的 48kHz/双声道/FLTP 帧；PTS 单位是 1/48000 秒。
 * read_window 的 target 由调用者预填静音，pts/nb_samples 指定需要覆盖的半开区间。
 * 例如目标 [0,1024)，源只有 [240,720)，则只拷贝 480 个采样，其他位置仍为静音。
 * filled 是累计填入的每声道采样数，首次设 0；等待后重试同一 target 时不要重新清零。
 * 返回 true 表示窗口边界已确定，可能包含静音；false 表示暂缺数据或等待到期。
 * 跨越窗口终点的源块保留在队首，下一窗口接着取；完全过期的块释放并计入 late_。
 * 所有队列状态由 mutex 保护；不要把锁内得到的内部帧裸指针保存到锁外。
 */
/*
 * 音频队列以采样窗口读取PCM，跨块拼接、裁掉窗口外数据；未覆盖位置保留预填静音。
 * 所有队列状态受同一锁保护，条件变量等待会释放锁，close同时唤醒生产者和消费者。
 */
#ifndef ESPORTS_QUEUE_AUDIO_QUEUE_H
#define ESPORTS_QUEUE_AUDIO_QUEUE_H
#include "codec/media_codec.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <chrono>
#include <algorithm>
namespace esports {
// 48kHz、双声道、FLTP；pts 为采样位置。队列与每块大小均有限制。
class AudioQueue {
public:
    explicit AudioQueue(size_t capacity = 32) : capacity_(capacity) {
        if (!capacity || capacity > 128) throw std::invalid_argument("音频队列块数无效");
    }
    // 按值取得帧所有权；即使关闭而拒绝入队，参数析构也会正确释放帧。
    // 满时等待不是忙轮询：wait 会释放锁，唤醒后重新检查“关闭或有空位”。
    bool push(Frame frame) {
        if (frame->format != AV_SAMPLE_FMT_FLTP || frame->sample_rate != 48000 ||
            frame->ch_layout.nb_channels != 2 || frame->nb_samples <= 0 || frame->nb_samples > 8192)
            throw std::runtime_error("音频队列帧格式/容量无效");
        std::unique_lock<std::mutex> lock(mutex_);
        writable_.wait(lock, [&] { return closed_ || frames_.size() < capacity_; });
        if (closed_) return false;
        frames_.push_back(std::move(frame));
        peak_ = std::max(peak_, frames_.size());
        readable_.notify_one();
        return true;
    }
    // 同一目标帧可重复调用直到覆盖/EOF；已读部分不会再次累加。
    // target 是可写的独立 PCM 帧，filled 是输入/输出累计值；本函数不接管 target。
    // wait_ms=0 供实时调度立即返回；正值供离线处理有限等待，超时后可保持原 target 重试。
    bool read_window(AVFrame* target, int& filled, int wait_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        // 半开区间[start,end)，单位为采样。调用者需先把target清为静音，filled初始为0。
        const int64_t start = target->pts, end = start + target->nb_samples;
        while (true) {
            while (!frames_.empty()) {
                AVFrame* source = frames_.front().get();
                const int64_t source_end = source->pts + source->nb_samples;
                if (source_end <= start) {
                    ++late_;
                    frames_.pop_front();
                    writable_.notify_one();
                    continue;
                }
                if (source->pts >= end) return true; // 未来帧证明当前间隙应为静音。
                // 两个半开区间的交集是 [max(起点), min(终点))；分别减源/目标起点得到数组偏移。
                const int64_t first = std::max(start, source->pts);
                const int64_t last = std::min(end, source_end);
                for (int channel = 0; channel < 2; ++channel) {
                    const float* in = reinterpret_cast<const float*>(source->extended_data[channel]);
                    float* out = reinterpret_cast<float*>(target->extended_data[channel]);
                    std::copy(in + first - source->pts, in + last - source->pts,
                              out + first - start);
                }
                filled += static_cast<int>(last - first);
                last_end_ = std::max(last_end_, source_end);
                if (source_end > end) return true;
                frames_.pop_front();
                writable_.notify_one();
            }
            if (closed_) return true;
            if (!wait_ms || readable_.wait_until(lock, deadline) == std::cv_status::timeout)
                return false;
        }
    }
    // 关闭入口并唤醒两侧；保留已经排队的帧供正常排空，允许重复 close。
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        readable_.notify_all();
        writable_.notify_all();
    }
    // 启动条件：有数据，或者生产者已明确结束。true 不等同于有可播放数据。
    bool ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ || !frames_.empty();
    }
    // 终止条件比 ready 更严格：生产者关闭且缓存为空，未来不会再有该路数据。
    bool exhausted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && frames_.empty();
    }
    size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return frames_.size(); }
    size_t peak() const { std::lock_guard<std::mutex> lock(mutex_); return peak_; }
    int64_t late() const { std::lock_guard<std::mutex> lock(mutex_); return late_; }
    int64_t last_end() const { std::lock_guard<std::mutex> lock(mutex_); return last_end_; }
private:
    // 容量按帧/块计，不按字节；mutex 保护容器、关闭标记与统计，const 查询也需加锁。
    const size_t capacity_;
    mutable std::mutex mutex_;
    // readable 在数据到达/关闭时唤醒消费者，writable 在空间出现/关闭时唤醒生产者。
    std::condition_variable writable_, readable_;
    std::deque<Frame> frames_;
    bool closed_ = false;
    size_t peak_ = 0;
    // late_ 数被整块丢掉的过期块；last_end_ 记录已覆盖源数据终点，供节目估算总结束时间。
    int64_t late_ = 0, last_end_ = 0;
};
}
#endif
