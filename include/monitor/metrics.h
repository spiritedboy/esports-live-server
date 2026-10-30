#ifndef ESPORTS_MONITOR_METRICS_H
#define ESPORTS_MONITOR_METRICS_H
#include "common/ffmpeg.h"
#include <array>
#include <cstdio>
namespace esports {
struct CodecMetrics {
    std::atomic<uint64_t> units{0}, calls{0}, total_us{0};
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
    void sample(const PipelineMetrics&);
private:
    FILE* file_ = nullptr;
    int64_t started_ = 0, previous_wall_ = 0, previous_cpu_ = 0;
    uint64_t previous_input_ = 0, previous_output_ = 0;
};
}
#endif
