/*
 * 格式适配：视频转YUV420P；单路转码音频经重采样和FIFO凑齐编码器需要的块。
 * 此处AudioConverter用于Phase 2；完整直播的时钟漂移处理见sync/audio_resampler.cpp。
 */
#include "codec/media_converter.h"
#include <algorithm>
#include <cstdlib>
namespace esports {
Frame VideoConverter::convert(const AVFrame* source, int width, int height) {
    if (source->format == AV_PIX_FMT_YUV420P && source->width == width && source->height == height) {
        Frame output = make_frame();
        // 新建帧外壳并增加像素引用，不拷贝整幅图；source销毁后output仍有效。
        // 若后续要改共享像素，先av_frame_make_writable；改PTS等外壳字段无需复制像素。
        ff_check(av_frame_ref(output.get(), source), "引用同格式视频帧");
        output->sample_aspect_ratio = AVRational{1, 1};
        // 共享只读像素，但不把输入I/P/B类型作为重新编码决策。
        output->pict_type = AV_PICTURE_TYPE_NONE;
        return output;
    }
    context_ = sws_getCachedContext(context_, source->width, source->height,
        static_cast<AVPixelFormat>(source->format), width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!context_) throw std::runtime_error("创建视频缩放器失败");
    Frame output = make_frame();
    output->format = AV_PIX_FMT_YUV420P;
    output->width = width;
    output->height = height;
    output->sample_aspect_ratio = AVRational{1, 1};
    ff_check(av_frame_get_buffer(output.get(), 32), "分配视频帧");
    const int rows = sws_scale(context_, source->data, source->linesize, 0,
                              source->height, output->data, output->linesize);
    if (rows != height) throw std::runtime_error("视频缩放行数异常");
    return output;
}
AudioConverter::~AudioConverter() {
    swr_free(&swr_);
    av_audio_fifo_free(fifo_);
    av_channel_layout_uninit(&input_layout_);
}
Frame AudioConverter::allocate(int samples) {
    Frame frame = make_frame();
    frame->format = target_->sample_fmt;
    frame->sample_rate = target_->sample_rate;
    frame->nb_samples = samples;
    ff_check(av_channel_layout_copy(&frame->ch_layout, &target_->ch_layout), "复制声道布局");
    ff_check(av_frame_get_buffer(frame.get(), 0), "分配音频帧");
    return frame;
}
// 重采样输出块长不一定等于AAC frame_size，FIFO把零散PCM拼成编码块。
// 最后一次允许送不足整块的尾部；next_pts_按实际送出的采样数累加。
void AudioConverter::drain(bool final, const FrameConsumer& consume) {
    const int block = target_->frame_size;
    if (block <= 0) throw std::runtime_error("AAC frame_size 无效");
    while (av_audio_fifo_size(fifo_) >= block || (final && av_audio_fifo_size(fifo_) > 0)) {
        const int samples = std::min(block, av_audio_fifo_size(fifo_));
        Frame output = allocate(samples);
        if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(output->extended_data), samples) != samples)
            throw std::runtime_error("读取音频 FIFO 失败");
        output->pts = next_pts_;
        next_pts_ += samples;
        consume(std::move(output));
    }
}
void AudioConverter::append(const AVFrame* frame, const FrameConsumer& consume) {
    if (frame->nb_samples <= 0) return;
    const int current = av_audio_fifo_size(fifo_);
    if (frame->nb_samples > 192000 - current)
        throw std::runtime_error("音频 FIFO 超过 4 秒容量限制");
    ff_check(av_audio_fifo_realloc(fifo_, current + frame->nb_samples), "扩展音频 FIFO");
    if (av_audio_fifo_write(fifo_, reinterpret_cast<void**>(frame->extended_data),
                            frame->nb_samples) != frame->nb_samples)
        throw std::runtime_error("写入音频 FIFO 失败");
    drain(false, consume);
}
void AudioConverter::send(const AVFrame* source, int64_t pts_samples, const FrameConsumer& consume) {
    if (!initialized_) {
        if (source->sample_rate <= 0 || !av_channel_layout_check(&source->ch_layout))
            throw std::runtime_error("输入音频参数无效");
        input_rate_ = source->sample_rate;
        input_format_ = source->format;
        ff_check(av_channel_layout_copy(&input_layout_, &source->ch_layout), "保存输入声道布局");
        ff_check(swr_alloc_set_opts2(&swr_, &target_->ch_layout, target_->sample_fmt,
            target_->sample_rate, &input_layout_, static_cast<AVSampleFormat>(input_format_),
            input_rate_, 0, nullptr), "创建重采样器");
        ff_check(swr_init(swr_), "初始化重采样器");
        fifo_ = av_audio_fifo_alloc(target_->sample_fmt, target_->ch_layout.nb_channels, 1);
        if (!fifo_) throw std::bad_alloc();
        next_pts_ = pts_samples;
        expected_input_pts_ = pts_samples;
        initialized_ = true;
    }
    if (input_rate_ != source->sample_rate || input_format_ != source->format ||
        av_channel_layout_compare(&input_layout_, &source->ch_layout) != 0)
        throw std::runtime_error("音频参数中途变化，当前阶段不动态重配置");
    if (std::llabs(pts_samples - expected_input_pts_) > 96)
        throw std::runtime_error("音频时间戳不连续，当前阶段不修复间断");
    expected_input_pts_ = pts_samples + av_rescale_q(source->nb_samples,
        AVRational{1, input_rate_}, target_->time_base);
    const int count = swr_get_out_samples(swr_, source->nb_samples);
    ff_check(count, "估算重采样输出");
    if (count <= 0 || count > 192000) throw std::runtime_error("重采样缓存超过限制");
    Frame output = allocate(count);
    const uint8_t** data = const_cast<const uint8_t**>(source->extended_data);
    output->nb_samples = swr_convert(swr_, output->extended_data, count, data, source->nb_samples);
    ff_check(output->nb_samples, "音频重采样");
    append(output.get(), consume);
}
void AudioConverter::finish(const FrameConsumer& consume) {
    if (!initialized_) return;
    while (true) {
        Frame tail = allocate(4096);
        tail->nb_samples = swr_convert(swr_, tail->extended_data, 4096, nullptr, 0);
        ff_check(tail->nb_samples, "排空重采样器");
        if (!tail->nb_samples) break;
        append(tail.get(), consume);
    }
    drain(true, consume);
}
}
