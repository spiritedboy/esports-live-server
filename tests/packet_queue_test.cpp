#include "queue/packet_queue.h"
#include <thread>
#include <atomic>
#include <iostream>
namespace {
esports::QueuedPacket item(bool video, bool key, int64_t time, int bytes = 40) {
    esports::QueuedPacket out;
    out.packet = esports::make_packet();
    esports::ff_check(av_new_packet(out.packet.get(), bytes), "分配测试包");
    out.packet->flags = key ? AV_PKT_FLAG_KEY : 0;
    out.bytes = bytes; out.video = video; out.dts_us = time;
    return out;
}
}
int main() {
    using namespace esports;
    PacketQueueLimits limits;
    limits.packets = 2; limits.bytes = 100; limits.latency_ms = 100;
    PacketQueue realtime(limits, true);
    realtime.push(item(false, false, 0));
    realtime.push(item(true, false, 0));
    if (realtime.snapshot().packets) return 1;
    realtime.push(item(true, true, 0));
    realtime.push(item(false, false, 20000));
    realtime.push(item(true, false, 40000)); // 包数超过2：整个窗口清除，非IDR不能进入。
    if (realtime.snapshot().packets || !realtime.take_key_request()) return 2;
    realtime.push(item(true, true, 80000));
    QueuedPacket old;
    realtime.pop(old);
    realtime.reset();
    if (realtime.current(old.epoch)) return 3;
    realtime.push(item(true, true, 100000));
    realtime.push(item(false, false, 250000)); // 媒体跨度超限，AAC不能成为恢复起点。
    if (realtime.snapshot().packets) return 4;
    realtime.push(item(true, true, 300000, 80));
    realtime.push(item(false, false, 310000, 40)); // 字节上限。
    if (realtime.snapshot().bytes) return 5;
    try { realtime.push(item(true, true, 0, 101)); return 6; }
    catch (const std::runtime_error&) {}
    const auto stats = realtime.snapshot();
    if (stats.peak_packets > 2 || stats.peak_bytes > 100 || !stats.dropped_audio || !stats.dropped_video)
        return 7;
    realtime.close();

    PacketQueue file(limits, false);
    file.push(item(true, true, 0)); file.push(item(false, false, 5000000));
    std::atomic<bool> finished(false);
    bool pushed = false;
    std::thread producer([&] { pushed = file.push(item(true, false, 9000000)); finished = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool blocked = !finished;
    file.pop(old);
    producer.join();
    file.close();
    int count = 0;
    while (file.pop(old)) ++count;
    if (!blocked || !pushed || count != 2 || file.snapshot().dropped_video || file.snapshot().dropped_audio)
        return 8;
    std::cout << "包数/字节/时间跨度上限、IDR门控、epoch失效、本地背压通过\n";
}
