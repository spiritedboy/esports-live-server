/*
 * 初学者导读
 * 录制是编码后的独立支路，不会再编码一次；空 prefix 表示完全禁用。
 * segment_ms 是期望分段时长，真正切段需要等视频关键帧，因此长度可能超过该值。
 * submit 借用包并克隆出独立外壳：录制与推流各改自己的 PTS/DTS，数据缓冲可以共享。
 * queue_bytes 是待处理编码数据上限；另有包数上限，防止磁盘慢导致内存持续增长。
 * 队列满或磁盘失败时 fail 关闭录制支路并记日志，主直播仍继续运行。
 * finish 关闭入口、唤醒工作线程并 join；正常情况排完队列后写尾，再登记索引。
 * mutex_ 保护队列、bytes_ 和关闭状态；耗时磁盘操作在工作线程锁外执行。
 */
/*
 * 独立录制线程及容量受限的编码包队列；空prefix禁用录制。
 * 主线程submit不等待磁盘写完，finish关闭入口后排空并join。
 */
#ifndef ESPORTS_RECORDING_RECORDER_H
#define ESPORTS_RECORDING_RECORDER_H
#include "output/stream_output.h"
#include "queue/packet_queue.h"
#include <thread>
namespace esports {
struct RecordingOptions {
    std::string prefix, format = "mp4";
    int segment_ms = 60000;
    size_t queue_bytes = 16 * 1024 * 1024;
};
// 单生产者；失败只关闭录制支路。每个输出拥有独立的包引用。
class Recorder {
public:
    Recorder(const RecordingOptions&, Logger&);
    ~Recorder();
    // 复制编码器的轨道描述并启动录制线程；打开失败记日志禁用该支路，不中止直播。
    void open(const std::vector<AVCodecContext*>&);
    // 仅借用传入包；内部 clone 引用后入队，调用者随后仍可把原包交给推流。
    void submit(const AVPacket*, AVRational);
    // 关闭队列入口并等线程排空；必须在所有 submit 完成后调用。
    void finish();
private:
    void run();
    // 统一失败出口：清空缓存、标记关闭并唤醒等待者；持队列锁时不可直接调用以免自锁。
    void fail(const std::string&);
    RecordingOptions options_;
    Logger& log_;
    AVFormatContext* description_ = nullptr;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedPacket> queue_;
    size_t bytes_ = 0;
    bool closed_ = false, failed_ = false;
};
}
#endif
