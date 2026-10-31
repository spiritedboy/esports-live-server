// 用微秒PTS验证选帧、容量背压和close唤醒；测试线程必须join后才能读取非原子结果。
#include "queue/frame_queue.h"
#include <atomic>
#include <iostream>
#include <thread>
namespace {
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
