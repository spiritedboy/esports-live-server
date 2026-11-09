/*
 * 初学者导读
 * 测试帧只设置 PTS，没有分配像素，因为这里测试的是调度和并发协议，不是图像处理。
 * 容量设为 2，第三次 push 放进另一个线程；消费一帧后生产者才应完成。
 * 再测试关闭满队列唤醒生产者、关闭空队列唤醒消费者，防止停机时 join 死锁。
 * 短 sleep 给线程进入等待的机会，不是严格的调度证明；done/entered 使用原子通信。
 * 非原子 accepted/rejected/awakened 都在 join 后读取，依赖 join 建立完成同步。
 */
// 用微秒PTS验证选帧、容量背压和close唤醒；测试线程必须join后才能读取非原子结果。
#include "queue/frame_queue.h"
#include <atomic>
#include <iostream>
#include <thread>
namespace {
// 仅生成带时间戳的外壳；队列选帧只看时间，不会访问像素。
esports::Frame frame(int64_t pts) {
    auto f = esports::make_frame();
    f->pts = pts;
    return f;
}
}
int main() {
    esports::FrameQueue queue(2);
    queue.push(frame(0));
    queue.push(frame(40000));
    std::atomic<bool> entered(false), done(false);
    bool accepted = false;
    std::thread producer([&] {
        entered.store(true);
        accepted = queue.push(frame(80000));
        done.store(true);
    });
    while (!entered) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool backpressure = !done;
    esports::Frame current;
    // 释放一个队列槽位后，阻塞的第三次 push 应能完成；join 后才读取 accepted。
    queue.advance(0, current);
    producer.join();
    const bool first = current && current->pts == 0;
    queue.close();
    const bool end = queue.advance(100000, current) && queue.exhausted();
    if (!backpressure || !accepted || !first || !end || current->pts != 80000 || queue.peak() != 2)
        return 1;

    // 满队列上的生产者必须被close唤醒，否则析构join可能永久等待。
    esports::FrameQueue closing(1);
    closing.push(frame(0));
    bool rejected = false;
    std::thread blocked([&] { rejected = !closing.push(frame(1)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    closing.close();
    blocked.join();
    if (!rejected) return 2;

    // 空队列消费者设 5 秒等待；close 应立即通知，避免必须等完整超时才能退出。
    esports::FrameQueue empty(1);
    bool awakened = false;
    std::thread consumer([&] {
        esports::Frame unused;
        awakened = empty.advance(0, unused, 5000);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    empty.close();
    consumer.join();
    if (!awakened) return 3;
    std::cout << "有界容量、背压、按时间选择和关闭唤醒测试通过\n";
    return 0;
}
