/*
 * 初学者导读
 * SourceClock 修补单个源的时间序列；rate 表示一秒包含多少刻度，不一定是视频帧率。
 * 视频用 rate=1000000，音频用 rate=48000；raw、duration、expected 都必须采用该单位。
 * expected_ 是按上一帧时长推算的下一起点，previous_ 是上一输出起点，bias_ 是累计平移。
 * 例如微秒序列 0、40000、缺失，duration=40000，则缺失项重建为 80000。
 * 遇到回退、重复或超过两秒的异常跳变，会将新段衔接至 expected_，累计 resets_。
 * container_origin_us 优先取容器共同起点；否则在轨道起点中取最早值，统一换成微秒。
 * 音视频必须减同一原点：若视频原来晚音频 100ms，分别减各自首帧会错误抹掉这 100ms。
 * 这只能处理媒体时间，不能自动知道两台独立摄像头实际拍摄时刻的差异。
 */
/*
 * 时间轴辅助：容器共同起点保留轨道偏移，SourceClock修补缺失/跳变时间戳。
 * 它不推断不同设备的绝对时钟关系；独立设备偏移仍需实测和配置。
 */
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
    // 输入 raw 可以缺失，duration 必须是本时基中的合理正时长。返回修补后的当前起点。
    // 这不是追赶墙钟：正常 raw 仍保留原来的源间偏移，只在异常时改变 bias。
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
        // bias只在不连续时更新，把新时间段接到expected_；正常帧保留原来的相对偏移。
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
// format 仅借用；容器 start_time 已是微秒，而各 stream.start_time 必须先按轨道时间基换算。
// 全部起点都缺失时返回 AV_NOPTS_VALUE，由输入模块决定首帧回退并说明同步限制。
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
