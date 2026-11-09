/*
 * 初学者导读
 * 重采样同时完成采样率转换、声道布局转换及采样格式转换，输出固定 FLTP/48kHz/双声道。
 * FLTP 的 P 表示 planar：左右声道分别位于 extended_data[0] 和 [1]，元素类型 float。
 * 预测输入位置 = 下一个输出位置 next_ + swr 内部尚未输出的延迟采样。
 * error 是源时间与预测时间之差：大于 24 采样（0.5ms）渐进补偿，大于 4800（100ms）重建。
 * 小误差用一秒的距离渐变消化，每次最多补偿 ±240 采样，避免突然删除整块声音。
 * 间断时 next_ 只向前移动；空出的时间段由 AudioQueue/read_window 保持静音。
 * swr_get_out_samples 给出容量上界，swr_convert 返回实际数量；推进 PTS 必须用实际数量。
 */
/*
 * 直播音频时钟适配：将不同采样率/布局转换为48kHz双声道FLTP。
 * 小误差通过swr_set_compensation逐渐修正；大间断重建重采样状态，队列负责补静音。
 */
#include "sync/audio_resampler.h"
#include "mixer/audio_mixer.h"
#include <algorithm>
#include <cstdlib>
namespace esports {
// swr 和保存的声道布局各有自己的资源；释放一个不会替代另一个的 uninit。
AudioResampler::~AudioResampler() {
    swr_free(&context_);
    av_channel_layout_uninit(&layout_);
}
void AudioResampler::send(const AVFrame* frame, int64_t pts, const FrameConsumer& consume) {
    if (frame->sample_rate <= 0 || frame->nb_samples <= 0 || frame->nb_samples > 8192 ||
        !av_channel_layout_check(&frame->ch_layout)) throw std::runtime_error("无效的输入音频帧");
    // 把输入 nb_samples/input_sample_rate 秒换成 48kHz 刻度，再交给 SourceClock 推算下一起点。
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
    // next_是下一个输出采样位置；还要加重采样器内部缓存，才是预期的输入时间位置。
    // 忽略滤波延迟会把正常缓存当成设备漂移。
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
        // 在48000个输出采样距离内渐变补偿，单次限制±240采样，避免直接跳PTS造成突变。
        const int delta = static_cast<int>(std::max<int64_t>(-240, std::min<int64_t>(240, error)));
        ff_check(swr_set_compensation(context_, delta, 48000), "补偿音频时钟漂移");
        ++stats_.corrections;
    }
    // 容量是上界而非实际产量；滤波延迟和软补偿都会使一次输入/输出采样数不相等。
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
// nullptr 输入表示“只输出内部尾巴”；持续调用直到返回 0，防止丢掉滤波器缓存采样。
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
// 组合两层统计：本层的重采样误差/补偿，和 SourceClock 的缺失/时间轴重建。
AudioSyncStats AudioResampler::stats() const {
    AudioSyncStats result = stats_;
    result.resets = clock_.resets();
    result.missing = clock_.missing();
    return result;
}
}
