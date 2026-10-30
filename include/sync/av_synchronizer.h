#ifndef ESPORTS_SYNC_AV_SYNCHRONIZER_H
#define ESPORTS_SYNC_AV_SYNCHRONIZER_H
#include "common/ffmpeg.h"
#include <algorithm>
#include <cstdlib>
namespace esports {
// 单线程源时钟：保留合法偏移；缺失时间戳重建，回退/超大跳变重建连续 epoch。
class SourceClock {
public:
    explicit SourceClock(int rate) : rate_(rate) {}
    int64_t map(int64_t raw, int64_t duration) {
        if (duration <= 0 || duration > int64_t(rate_) * 2)
            throw std::runtime_error("源帧时长超出范围");
        if (raw != AV_NOPTS_VALUE && (raw < -(int64_t(1) << 50) || raw > (int64_t(1) << 50)))
            throw std::runtime_error("源时间戳超出范围");
        if (raw == AV_NOPTS_VALUE) {
            ++missing_;
            const int64_t value = initialized_ ? expected_ : 0;
            previous_ = value;
            expected_ = value + duration;
            initialized_ = true;
            return value;
        }
        int64_t mapped = raw + bias_;
        if (initialized_ && (mapped <= previous_ || std::llabs(mapped - expected_) > int64_t(rate_) * 2)) {
            bias_ += expected_ - mapped;
            mapped = expected_;
            ++resets_;
        }
        previous_ = mapped;
        expected_ = mapped + duration;
        initialized_ = true;
        return mapped;
    }
    int64_t missing() const { return missing_; }
    int64_t resets() const { return resets_; }
private:
    int rate_;
    bool initialized_ = false;
    int64_t expected_ = 0, previous_ = 0, bias_ = 0, missing_ = 0, resets_ = 0;
};
inline int64_t container_origin_us(const AVFormatContext* format) {
    if (format->start_time != AV_NOPTS_VALUE) return format->start_time;
    int64_t origin = AV_NOPTS_VALUE;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* s = format->streams[i];
        if (s->start_time == AV_NOPTS_VALUE) continue;
        const int64_t value = av_rescale_q(s->start_time, s->time_base, AVRational{1, AV_TIME_BASE});
        if (origin == AV_NOPTS_VALUE || value < origin) origin = value;
    }
    return origin;
}
}
#endif
