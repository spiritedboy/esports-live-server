/*
 * 编码包队列是直播延迟控制的边界：本地背压保数据，实时网络清旧窗口等新关键帧。
 * epoch是缓存代次；出队后仍可能失效，消费者写入前必须再次核对。
 */
#ifndef ESPORTS_QUEUE_PACKET_QUEUE_H
#define ESPORTS_QUEUE_PACKET_QUEUE_H
#include "common/ffmpeg.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <algorithm>
namespace esports {
struct QueuedPacket {
    Packet packet;
    AVRational time_base{0, 1};
    // dts_us属于媒体时间轴；queued_us属于单调墙钟，用来测真实排队耗时，不能相减混用。
    int64_t dts_us = 0, queued_us = 0;
    uint64_t epoch = 0;
    bool video = false;
    size_t bytes = 0;
};
struct PacketQueueStats {
    uint64_t dropped_video = 0, dropped_audio = 0, resets = 0;
    size_t packets = 0, bytes = 0, peak_packets = 0, peak_bytes = 0;
};
struct PacketQueueLimits {
    size_t packets = 256, bytes = 8 * 1024 * 1024;
    int latency_ms = 500;
};
// 本地文件采用背压；实时网络丢弃整个过期窗口，直到新的关键帧。
class PacketQueue {
public:
    PacketQueue(PacketQueueLimits limits, bool realtime) : limits_(limits), realtime_(realtime),
        waiting_key_(realtime), key_requested_(realtime) {
        if (!limits.packets || !limits.bytes || limits.latency_ms <= 0)
            throw std::invalid_argument("编码队列上限无效");
    }
    bool push(QueuedPacket item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (item.bytes > limits_.bytes) throw std::runtime_error("单个编码包超过队列字节上限");
        if (realtime_) {
            if (overflow(item)) reset_locked();
            if (waiting_key_) {
                if (!item.video || !(item.packet->flags & AV_PKT_FLAG_KEY)) {
                    dropped_locked(item);
                    return !closed_;
                }
                waiting_key_ = false;
                // key_requested_ 由生产者取走；接受一个已经生成的 IDR 也可结束等待。
                key_requested_ = false;
            }
        } else {
            writable_.wait(lock, [&] { return closed_ || !overflow(item); });
        }
        if (closed_) return false;
        item.epoch = epoch_;
        queue_.push_back(std::move(item));
        stats_.bytes += queue_.back().bytes;
        stats_.packets = queue_.size();
        stats_.peak_packets = std::max(stats_.peak_packets, queue_.size());
        stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.bytes);
        readable_.notify_one();
        return true;
    }
    bool pop(QueuedPacket& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        readable_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        stats_.bytes -= item.bytes;
        stats_.packets = queue_.size();
        writable_.notify_all();
        return true;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_locked();
    }
    void close(bool discard = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (discard) {
            for (const auto& item : queue_) dropped_locked(item);
            queue_.clear();
            stats_.bytes = stats_.packets = 0;
        }
        closed_ = true;
        readable_.notify_all(); writable_.notify_all();
    }
    bool current(uint64_t epoch) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return epoch == epoch_;
    }
    void dropped(const QueuedPacket& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped_locked(item);
    }
    bool take_key_request() {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool value = key_requested_;
        key_requested_ = false;
        return value;
    }
    PacketQueueStats snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
private:
    bool overflow(const QueuedPacket& item) const {
        if (queue_.size() >= limits_.packets || item.bytes > limits_.bytes - stats_.bytes) return true;
        if (!realtime_ || queue_.empty()) return false;
        int64_t low = item.dts_us, high = item.dts_us;
        for (const auto& value : queue_) { low = std::min(low, value.dts_us); high = std::max(high, value.dts_us); }
        return high - low > int64_t(limits_.latency_ms) * 1000;
    }
    void dropped_locked(const QueuedPacket& item) {
        if (item.video) ++stats_.dropped_video; else ++stats_.dropped_audio;
    }
    // 整批丢弃并递增代次，使已被消费者拿走的旧包也失效。
    // 不能只丢任意P帧后继续发：后续参考链可能已断，需要新的IDR重新开始。
    void reset_locked() {
        for (const auto& item : queue_) dropped_locked(item);
        queue_.clear();
        stats_.bytes = stats_.packets = 0;
        ++epoch_; ++stats_.resets;
        waiting_key_ = key_requested_ = true;
        writable_.notify_all();
    }
    PacketQueueLimits limits_;
    bool realtime_, waiting_key_, key_requested_, closed_ = false;
    uint64_t epoch_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable readable_, writable_;
    std::deque<QueuedPacket> queue_;
    PacketQueueStats stats_;
};
}
#endif
