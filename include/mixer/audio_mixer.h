/*
 * 初学者导读
 * audio_frame 创建拥有缓冲的静音帧；samples 是每声道采样数，pts 是 48kHz 采样位置。
 * AudioMixer 只混合同一时间窗口，格式统一与时间对齐分别由重采样器、音频队列完成。
 * frames 是借用的只读指针列表，返回 Frame 拥有新输出缓冲，不修改各输入音频。
 * gain 使用线性幅度：0 静音，1 原幅度，0.5 半幅度；这不是“0.5 分贝”。
 * Gain.value 保存设置值，current 是当前实际值，target 是考虑静音后的目标值。
 * step/remaining 实现 240 个采样即 5ms 的过渡，避免音量骤变产生爆音。
 * limiter_ 是两声道共用的峰值衰减系数，limited_ 统计触发保护的采样位置数。
 * 本类没有锁，set_gain/set_muted/mix 都必须在节目线程中调用。
 */
/*
 * 混音要求输入均为48kHz/双声道/FLTP且覆盖同一PTS与采样数。
 * 增益为线性幅度，不是分贝；set_gain和set_muted也由混音线程调用。
 */
#ifndef ESPORTS_MIXER_AUDIO_MIXER_H
#define ESPORTS_MIXER_AUDIO_MIXER_H
#include "codec/media_codec.h"
#include <vector>
namespace esports {
// 返回已分配并填静音的立体声 FLTP 帧；samples 范围 1..8192，pts 单位 1/48000 秒。
Frame audio_frame(int samples, int64_t pts);
class AudioMixer {
public:
    explicit AudioMixer(size_t sources);
    // 设置指定源的目标线性增益，范围 0..4；用短过渡渐变到目标，不突改当前幅度。
    void set_gain(size_t index, double gain);
    // 静音只把目标设为零，保留用户设置的 gain，取消静音时恢复该目标增益。
    void set_muted(size_t index, bool muted);
    // frames 数量等于构造时源数，且每帧格式、PTS、采样数必须完全匹配。失败抛异常。
    Frame mix(const std::vector<const AVFrame*>& frames, int64_t pts, int samples);
    int64_t limited_samples() const { return limited_; }
private:
    struct Gain { double value = 1, current = 0, target = 1, step = 0; int remaining = 240; bool muted = false; };
    void update(size_t index);
    std::vector<Gain> gains_;
    double limiter_ = 1;
    int64_t limited_ = 0;
};
}
#endif
