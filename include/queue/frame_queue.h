/*
 * 初学者导读
 * 数据流：一个 VideoSource 生产 → push 转移帧所有权 → 节目线程 advance 选帧。
 * 队列内 pts/duration 都用微秒；tick 参数也是微秒，不是 run_live 的视频帧序号。
 * 例如队列 PTS 为 0、40000、80000，advance(60000) 会把 current 更新为 40000。
 * 80000 留在队列等未来；0 被替换时自动释放。current 可跨多次调度保留上一张画面。
 * advance 返回 true 只表示已有足够边界信息（未来帧或关闭），不保证 current 非空。
 * push 在满队列上等待；条件变量等待期间释放 mutex，使消费者有机会腾出空间。
 * close 是终止标记，不清空已有帧；ready 用于启动判断，exhausted 用于最终结束判断。
 */
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
    // 按值取得帧所有权；即使关闭而拒绝入队，参数析构也会正确释放帧。
    // 满时等待不是忙轮询：wait 会释放锁，唤醒后重新检查“关闭或有空位”。
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
    // current 是调用方持有的上一帧，本函数可能多次替换它；不等待比 tick 更晚的画面。
    // 返回 false 也可能已更新 current，只是还没看到未来帧或明确结束的边界。
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
    // 关闭入口并唤醒两侧；保留已经排队的帧供正常排空，允许重复 close。
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        writable_.notify_all();
        readable_.notify_all();
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
    size_t peak() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_;
    }
private:
    // 容量按帧/块计，不按字节；mutex 保护容器、关闭标记与统计，const 查询也需加锁。
    const size_t capacity_;
    mutable std::mutex mutex_;
    // readable 在数据到达/关闭时唤醒消费者，writable 在空间出现/关闭时唤醒生产者。
    std::condition_variable readable_, writable_;
    std::deque<Frame> frames_;
    bool closed_ = false;
    size_t peak_ = 0;
};
}
#endif
