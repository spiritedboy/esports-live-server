/*
 * 初学者导读
 * CodecMetrics 放在工作线程附近统计调用次数、成功产物数量和 FFmpeg API 累计耗时。
 * units 在解码器代表帧数，在编码器代表包数，不能把两种计数直接当作一一对应。
 * measure 返回原始 FFmpeg 状态码，让调用者继续处理 EAGAIN/EOF；它不吞掉失败。
 * PipelineMetrics 是主线程收集到的普通数值快照，最多记录四路视频和四路音频。
 * MetricsFile 用这些快照计算区间 FPS、CPU 占用，并附上内存、队列与丢弃统计。
 * 空路径禁用文件；非空路径使用排他创建，避免覆盖用户已有报告。
 * 原子读取解决数据竞争，但不把多个指标变成同一时刻的事务快照。
 */
/*
 * 跨线程计数用原子变量，单次指标快照不保证所有字段来自同一瞬间。
 * API累计耗时仅包围send/receive调用，不含回调处理，也不等同于编解码后台线程CPU总量。
 */
#ifndef ESPORTS_MONITOR_METRICS_H
#define ESPORTS_MONITOR_METRICS_H
#include "common/ffmpeg.h"
#include <array>
#include <cstdio>
namespace esports {
struct CodecMetrics {
    std::atomic<uint64_t> units{0}, calls{0}, total_us{0};
    // 用单调时钟包围一次 FFmpeg API 调用并累积，返回值不改变。后台线程 CPU 用量另行统计。
    template<class Operation> int measure(Operation operation) {
        const int64_t start = av_gettime_relative();
        const int result = operation();
        total_us.fetch_add(av_gettime_relative() - start, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
};
struct PipelineMetrics {
    std::array<uint64_t, 4> video_frames{{0,0,0,0}}, audio_frames{{0,0,0,0}};
    uint64_t decode_us = 0, encode_us = 0, processing_us = 0;
    size_t video_queued = 0, audio_queued = 0;
    uint64_t audio_late = 0, output_video = 0, output_dropped = 0;
    int64_t skipped_video = 0, skipped_audio = 0;
};
class MetricsFile {
public:
    explicit MetricsFile(const std::string& path);
    ~MetricsFile();
    bool enabled() const { return file_ != nullptr; }
    // 主线程传入一次快照，计算与上一次的差值并写 CSV；文件未启用时无操作。
    void sample(const PipelineMetrics&);
private:
    FILE* file_ = nullptr;
    // started_ 是启动墙钟原点；previous_* 是上一采样点，用于计算区间速率而非全程平均。
    int64_t started_ = 0, previous_wall_ = 0, previous_cpu_ = 0;
    uint64_t previous_input_ = 0, previous_output_ = 0;
};
}
#endif
