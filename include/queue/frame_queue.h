/*
 * 视频队列连接输入线程与节目调度线程；有界容量让快生产者等待慢消费者。
 * 选帧按节目微秒时间，而不是按入队顺序每次只取一帧。
 */
#ifndef ESPORTS_QUEUE_FRAME_QUEUE_H
#define ESPORTS_QUEUE_FRAME_QUEUE_H
#include "codec/media_codec.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <chrono>
namespace esports {
// 单生产者/单消费者，帧 pts/duration 单位均为微秒。关闭可唤醒所有等待者。
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity) : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("帧队列容量必须大于零");
    }
    bool push(Frame frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        writable_.wait(lock, [&] { return closed_ || frames_.size() < capacity_; });
        if (closed_) return false;
        frames_.push_back(std::move(frame));
        if (frames_.size() > peak_) peak_ = frames_.size();
        readable_.notify_one();
        return true;
    }
    // 更新至 <= tick 的最新帧。返回 true 表示已看到未来帧或 EOF。
    bool advance(int64_t tick, Frame& current, int wait_ms = 0) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        while (true) {
            while (!frames_.empty() && frames_.front()->pts <= tick) {
                // 一次消费所有不晚于tick的帧，只保留最新一张；被替换帧由RAII释放。
                current = std::move(frames_.front());
                frames_.pop_front();
                writable_.notify_one();
            }
            if (!frames_.empty() || closed_) return true;
            if (!wait_ms || readable_.wait_until(lock, deadline) == std::cv_status::timeout)
                return false;
        }
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        writable_.notify_all();
        readable_.notify_all();
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
    size_t peak() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_;
    }
private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable readable_, writable_;
    std::deque<Frame> frames_;
    bool closed_ = false;
    size_t peak_ = 0;
};
}
#endif
