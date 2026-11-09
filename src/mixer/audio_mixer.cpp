/*
 * 初学者导读
 * 先看 audio_frame：设置元信息 → 分配音频缓冲 → 填零静音；只 alloc 外壳还不能写采样。
 * 再看 mix：检查所有输入窗口相同 → 每采样更新增益 → 同声道求和 → 左右联动限幅。
 * 假设两路采样分别为 0.25 和 0.5，增益为 0.5 和 0，则过渡结束后输出为 0.125。
 * 如果求和峰值超过 0.95，则共同缩小左右声道，避免只压一边造成声像移动。
 * 这是一种简单峰值保护，没有实现响度测量、均衡器或专业动态压缩器。
 * 调用者保留各输入 Frame 的所有权；mix 读取裸指针并返回独立分配的新 Frame。
 */
/*
 * 混音器接收已对齐到同一采样窗口的PCM：逐源增益 → 求和 → 双声道联动限幅。
 * 不在此解码或调整时间轴；增益状态与mix均由主处理线程访问。
 */
#include "mixer/audio_mixer.h"
#include <algorithm>
#include <cmath>
extern "C" {
#include <libavutil/samplefmt.h>
}
namespace esports {
Frame audio_frame(int samples, int64_t pts) {
    if (samples <= 0 || samples > 8192) throw std::runtime_error("音频块大小超出范围");
    Frame frame = make_frame();
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = 48000;
    frame->nb_samples = samples;
    frame->pts = pts;
    av_channel_layout_default(&frame->ch_layout, 2);
    ff_check(av_frame_get_buffer(frame.get(), 0), "分配混音帧");
    ff_check(av_samples_set_silence(frame->extended_data, 0, samples, 2, AV_SAMPLE_FMT_FLTP),
             "初始化音频静音");
    return frame;
}
AudioMixer::AudioMixer(size_t sources) : gains_(sources) {
    if (!sources || sources > 4) throw std::runtime_error("混音支持 1–4 路");
    for (size_t i = 0; i < sources; ++i) update(i);
}
// 每次重新设置目标都从当前实际增益渐变，连续多次调整也不会瞬间跳到旧目标。
void AudioMixer::update(size_t index) {
    Gain& gain = gains_.at(index);
    gain.target = gain.muted ? 0 : gain.value;
    gain.remaining = 240; // 5ms 线性过渡，减少调音量造成的阶跃。
    gain.step = (gain.target - gain.current) / gain.remaining;
}
void AudioMixer::set_gain(size_t index, double value) {
    if (!std::isfinite(value) || value < 0 || value > 4) throw std::runtime_error("音量必须为 0–4");
    gains_.at(index).value = value;
    update(index);
}
void AudioMixer::set_muted(size_t index, bool value) {
    gains_.at(index).muted = value;
    update(index);
}
Frame AudioMixer::mix(const std::vector<const AVFrame*>& frames, int64_t pts, int samples) {
    if (frames.size() != gains_.size()) throw std::runtime_error("混音输入数不匹配");
    for (const auto* frame : frames) {
        if (!frame || frame->format != AV_SAMPLE_FMT_FLTP || frame->ch_layout.nb_channels != 2 ||
            frame->sample_rate != 48000 || frame->nb_samples != samples || frame->pts != pts)
            throw std::runtime_error("混音输入未按采样时间对齐");
    }
    Frame output = audio_frame(samples, pts);
    // 最外层按时间采样位置推进，确保两个声道使用同一个瞬时增益和限幅系数。
    for (int sample = 0; sample < samples; ++sample) {
        double sum[2] = {};
        for (size_t i = 0; i < frames.size(); ++i) {
            Gain& gain = gains_[i];
            if (gain.remaining > 0) {
                gain.current += gain.step;
                if (--gain.remaining == 0) gain.current = gain.target;
            }
            for (int channel = 0; channel < 2; ++channel) {
                // extended_data[channel] 是该声道 float 数组；不能按交错 LRLR 的 packed 布局解释。
                const float value = reinterpret_cast<const float*>(frames[i]->extended_data[channel])[sample];
                if (!std::isfinite(value)) throw std::runtime_error("PCM 包含非有限采样");
                sum[channel] += value * gain.current;
            }
        }
        // 两声道使用同一限幅系数，尽量保持左右声像；这是简单峰值保护，不是响度标准化。
        const double peak = std::max(std::abs(sum[0]), std::abs(sum[1]));
        const double needed = peak > .95 ? .95 / peak : 1;
        limiter_ = std::min(needed, limiter_ + 1.0 / 4800); // 快速抑制、约100ms恢复。
        if (limiter_ < .999) ++limited_;
        for (int channel = 0; channel < 2; ++channel)
            reinterpret_cast<float*>(output->extended_data[channel])[sample] =
                static_cast<float>(sum[channel] * limiter_);
    }
    return output;
}
}
