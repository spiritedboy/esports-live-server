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
    for (int sample = 0; sample < samples; ++sample) {
        double sum[2] = {};
        for (size_t i = 0; i < frames.size(); ++i) {
            Gain& gain = gains_[i];
            if (gain.remaining > 0) {
                gain.current += gain.step;
                if (--gain.remaining == 0) gain.current = gain.target;
            }
            for (int channel = 0; channel < 2; ++channel) {
                const float value = reinterpret_cast<const float*>(frames[i]->extended_data[channel])[sample];
                if (!std::isfinite(value)) throw std::runtime_error("PCM 包含非有限采样");
                sum[channel] += value * gain.current;
            }
        }
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
