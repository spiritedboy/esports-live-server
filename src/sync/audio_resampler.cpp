#include "sync/audio_resampler.h"
#include "mixer/audio_mixer.h"
#include <algorithm>
#include <cstdlib>
namespace esports {
AudioResampler::~AudioResampler() {
    swr_free(&context_);
    av_channel_layout_uninit(&layout_);
}
void AudioResampler::send(const AVFrame* frame, int64_t pts, const FrameConsumer& consume) {
    if (frame->sample_rate <= 0 || frame->nb_samples <= 0 || frame->nb_samples > 8192 ||
        !av_channel_layout_check(&frame->ch_layout)) throw std::runtime_error("无效的输入音频帧");
    const int64_t duration = av_rescale_q(frame->nb_samples, AVRational{1, frame->sample_rate},
                                         AVRational{1, 48000});
    pts = clock_.map(pts, duration);
    if (!context_) {
        rate_ = frame->sample_rate;
        format_ = frame->format;
        ff_check(av_channel_layout_copy(&layout_, &frame->ch_layout), "保存重采样布局");
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        ff_check(swr_alloc_set_opts2(&context_, &stereo, AV_SAMPLE_FMT_FLTP, 48000,
            &layout_, static_cast<AVSampleFormat>(format_), rate_, 0, nullptr), "创建同步重采样器");
        ff_check(swr_init(context_), "初始化同步重采样器");
        next_ = pts;
    }
    if (rate_ != frame->sample_rate || format_ != frame->format ||
        av_channel_layout_compare(&layout_, &frame->ch_layout))
        throw std::runtime_error("音频参数中途变化");
    const int64_t predicted = next_ + swr_get_delay(context_, 48000);
    const int64_t error = pts - predicted;
    stats_.max_error = std::max(stats_.max_error, static_cast<int64_t>(std::llabs(error)));
    if (std::llabs(error) > 4800) {
        // >100ms 属于间断；清掉旧滤波延迟，以新的时间位置留出静音区间。
        swr_close(context_);
        ff_check(swr_init(context_), "间断后重建重采样器");
        next_ = std::max(next_, pts);
        ++stats_.gaps;
    } else if (std::llabs(error) > 24) {
        const int delta = static_cast<int>(std::max<int64_t>(-240, std::min<int64_t>(240, error)));
        ff_check(swr_set_compensation(context_, delta, 48000), "补偿音频时钟漂移");
        ++stats_.corrections;
    }
    const int capacity = swr_get_out_samples(context_, frame->nb_samples);
    ff_check(capacity, "计算同步音频缓存");
    if (capacity <= 0 || capacity > 8192) throw std::runtime_error("同步重采样缓存超过上限");
    Frame output = audio_frame(capacity, next_);
    output->nb_samples = swr_convert(context_, output->extended_data, capacity,
        const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    ff_check(output->nb_samples, "同步音频重采样");
    next_ += output->nb_samples;
    if (output->nb_samples) consume(std::move(output));
}
void AudioResampler::finish(const FrameConsumer& consume) {
    if (!context_) return;
    while (true) {
        Frame output = audio_frame(4096, next_);
        output->nb_samples = swr_convert(context_, output->extended_data, 4096, nullptr, 0);
        ff_check(output->nb_samples, "排空同步重采样器");
        if (!output->nb_samples) break;
        next_ += output->nb_samples;
        consume(std::move(output));
    }
}
AudioSyncStats AudioResampler::stats() const {
    AudioSyncStats result = stats_;
    result.resets = clock_.resets();
    result.missing = clock_.missing();
    return result;
}
}
