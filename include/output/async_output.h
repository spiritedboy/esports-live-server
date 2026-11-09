/*
 * 初学者导读
 * 主线程只提交编码包；run 在独立线程调用 PacketSink，避免网络写阻塞合成和混音。
 * open 复制编码参数后启动线程，真实连接在工作线程收到可用包时建立。
 * submit 按值接管 Packet；调用者用 std::move 交接后，不再通过旧指针访问该包。
 * PacketSink 是生产与故障测试的接缝：默认实现是真实 FFmpeg 封装，测试可以注入慢写。
 * finish 关闭入口并等待排空；cancel 丢弃积压、打断 I/O 和重连退避；两者含义不同。
 * 工作线程异常无法直接穿过线程边界，因此保存 error_，由主线程 check/finish 重新抛出。
 * description_ 只保存轨道参数快照，不是网络连接；worker_ 结束后才能释放它。
 * mutex_ 保护异常和统计，queue_ 有自己的锁；不要同时持有两个锁做阻塞 I/O。
 */
/*
 * 异步推流边界：主线程open/submit/check/finish，工作线程独占PacketSink。
 * 工作线程异常保存在exception_ptr，由主线程check/finish重新抛出。
 */
#ifndef ESPORTS_OUTPUT_ASYNC_OUTPUT_H
#define ESPORTS_OUTPUT_ASYNC_OUTPUT_H
#include "queue/packet_queue.h"
#include "output/stream_output.h"
#include <functional>
#include <thread>
namespace esports {
// timeout_ms 是每次输出 I/O 超时；backoff_ms 是首次重试等待，后续增长但最多 4 秒。
struct OutputOptions {
    PacketQueueLimits queue;
    int timeout_ms = 3000;
    int reconnect_attempts = 5;
    int backoff_ms = 250;
};
// 成功包数/字节数在写入成功后累计；attempts 是打开次数，sessions 是打开成功次数。
// max_queue_us、max_write_us 是历史最大墙钟耗时，elapsed_us 是输出对象运行时长。
struct OutputStats {
    uint64_t video_packets = 0, audio_packets = 0, encoded_bytes = 0;
    uint64_t attempts = 0, sessions = 0, failures = 0, retries = 0;
    int64_t elapsed_us = 0, max_queue_us = 0, max_write_us = 0;
    PacketQueueStats queue;
};
// 可替换边界仅用于故障注入测试；生产默认始终使用 FFmpeg 实际输出。
class PacketSink {
public:
    virtual ~PacketSink() {}
    virtual void open(const std::string&, AVFormatContext*, Logger&) = 0;
    virtual void write(AVPacket*, AVRational, int64_t origin_us) = 0;
    virtual void finish() = 0;
};
typedef std::function<std::unique_ptr<PacketSink>(IoControl&)> SinkFactory;
class AsyncOutput {
public:
    AsyncOutput(const OutputOptions&, bool network, Logger&, SinkFactory factory = SinkFactory());
    ~AsyncOutput();
    AsyncOutput(const AsyncOutput&) = delete;
    AsyncOutput& operator=(const AsyncOutput&) = delete;
    // 复制编码参数快照并启动 worker；要求 codecs 顺序为 H.264 视频、AAC 音频。
    void open(const std::string&, const std::vector<AVCodecContext*>&);
    // 接管Packet；调用后原unique_ptr为空。网络可丢过期数据，本地队列满会背压等待。
    void submit(Packet, AVRational);
    bool take_keyframe_request() { return queue_.take_key_request(); }
    void reset_for_latency() { if (network_) queue_.reset(); }
    // 主线程定期调用以发现异步失败；若工作线程存有异常，这里在主线程重新抛出。
    void check() const;
    // 正常结束时停止收新包并等待排空；若传入停止标志，则等待期间仍可响应 Ctrl+C。
    void finish(const volatile std::sig_atomic_t* user_stop = nullptr);
    // 主动终止：原子停止位 + 丢弃队列 + 唤醒退避。调用后仍需 finish/join 等待线程结束。
    void cancel();
    // 返回数值副本，队列统计和输出统计分别加锁获取，可能不是完全同一瞬间。
    OutputStats snapshot() const;
private:
    void run(std::string url);
    OutputOptions options_;
    bool network_;
    Logger& log_;
    SinkFactory factory_;
    PacketQueue queue_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    // 拥有仅用于参数快照的上下文；不是正在编码的 AVCodecContext，避免跨线程共用编码状态。
    AVFormatContext* description_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    // 保存工作线程最外层捕获的异常；读写受 mutex_ 保护，避免异常静默丢失。
    std::exception_ptr error_;
    OutputStats stats_;
    int64_t started_us_ = 0, ended_us_ = 0;
    bool finished_ = false;
};
}
#endif
