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
    bool read_window(AVFrame* target, int& filled, int wait_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
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
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        readable_.notify_all();
        writable_.notify_all();
    }
    bool ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ || !frames_.empty();
    }
    bool exhausted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && frames_.empty();
    }
    size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return frames_.size(); }
    size_t peak() const { std::lock_guard<std::mutex> lock(mutex_); return peak_; }
    int64_t late() const { std::lock_guard<std::mutex> lock(mutex_); return late_; }
    int64_t last_end() const { std::lock_guard<std::mutex> lock(mutex_); return last_end_; }
private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable writable_, readable_;
    std::deque<Frame> frames_;
    bool closed_ = false;
    size_t peak_ = 0;
    int64_t late_ = 0, last_end_ = 0;
};
}
#endif
