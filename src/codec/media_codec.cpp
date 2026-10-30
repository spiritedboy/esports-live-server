#include "codec/media_codec.h"
extern "C" {
#include <libavutil/opt.h>
}
namespace esports {
void MediaDecoder::open(const AVStream* stream) {
    if (context_) throw std::logic_error("解码器已打开");
    const AVCodecID id = stream->codecpar->codec_id;
    if (id != AV_CODEC_ID_H264 && id != AV_CODEC_ID_HEVC && id != AV_CODEC_ID_AAC && id != AV_CODEC_ID_OPUS)
        throw std::runtime_error("仅支持 H.264/H.265/AAC/Opus 解码");
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) throw std::runtime_error("找不到解码器");
    context_.reset(avcodec_alloc_context3(codec));
    if (!context_) throw std::bad_alloc();
    ff_check(avcodec_parameters_to_context(context_.get(), stream->codecpar), "复制解码参数");
    context_->pkt_timebase = stream->time_base;
    context_->thread_count = 2;
    context_->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
    ff_check(avcodec_open2(context_.get(), codec, nullptr), "打开解码器");
}
int MediaDecoder::drain(const FrameConsumer& consume) {
    while (true) {
        Frame frame = make_frame();
        const int result = metrics_->measure([&] { return avcodec_receive_frame(context_.get(), frame.get()); });
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
        ff_check(result, "接收解码帧");
        if (frame->flags & AV_FRAME_FLAG_CORRUPT) throw std::runtime_error("损坏的解码帧");
        frame->pts = frame->best_effort_timestamp;
        metrics_->units.fetch_add(1, std::memory_order_relaxed);
        consume(std::move(frame));
    }
}
void MediaDecoder::send(const AVPacket* packet, const FrameConsumer& consume) {
    if (finished_) throw std::logic_error("解码器已排空");
    int result = metrics_->measure([&] { return avcodec_send_packet(context_.get(), packet); });
    if (result == AVERROR(EAGAIN)) {
        drain(consume);
        result = metrics_->measure([&] { return avcodec_send_packet(context_.get(), packet); });
    }
    ff_check(result, "发送解码包");
    const int terminal = drain(consume);
    if (!packet) {
        if (terminal != AVERROR_EOF) throw std::runtime_error("解码器排空未到 EOF");
        finished_ = true;
    }
}
void MediaEncoder::open_video(int width, int height, int fps, const VideoEncodingOptions& options) {
    if (context_) throw std::logic_error("编码器已打开");
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) throw std::runtime_error("缺少 libx264 编码器");
    context_.reset(avcodec_alloc_context3(codec));
    if (!context_) throw std::bad_alloc();
    context_->width = width;
    context_->height = height;
    context_->pix_fmt = AV_PIX_FMT_YUV420P;
    context_->time_base = AVRational{1, 90000};
    context_->framerate = AVRational{fps, 1};
    if (options.bitrate < 64000 || options.bitrate > 20000000 ||
        options.vbv_ms < 50 || options.vbv_ms > 2000 || options.gop_frames < 0)
        throw std::runtime_error("视频编码参数超出范围");
    context_->bit_rate = options.bitrate;
    context_->gop_size = options.gop_frames ? options.gop_frames : fps * 2;
    if (options.low_latency) {
        context_->rc_max_rate = options.bitrate;
        context_->rc_buffer_size = static_cast<int>(int64_t(options.bitrate) * options.vbv_ms / 1000);
        context_->rc_initial_buffer_occupancy = context_->rc_buffer_size / 2;
        context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ff_check(av_opt_set(context_->priv_data, "forced-idr", "1", 0), "启用强制 IDR");
        ff_check(av_opt_set(context_->priv_data, "x264-params",
            "scenecut=0:rc-lookahead=0:sync-lookahead=0", 0), "设置编码低延迟参数");
    }
    context_->max_b_frames = 0;
    context_->thread_count = 2;
    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ff_check(av_opt_set(context_->priv_data, "preset", "veryfast", 0), "设置 x264 preset");
    ff_check(av_opt_set(context_->priv_data, "tune", "zerolatency", 0), "设置 x264 tune");
    ff_check(avcodec_open2(context_.get(), codec, nullptr), "打开 H.264 编码器");
}
void MediaEncoder::open_audio() {
    if (context_) throw std::logic_error("编码器已打开");
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) throw std::runtime_error("缺少 AAC 编码器");
    context_.reset(avcodec_alloc_context3(codec));
    if (!context_) throw std::bad_alloc();
    context_->sample_rate = 48000;
    context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&context_->ch_layout, 2);
    context_->time_base = AVRational{1, 48000};
    context_->bit_rate = 128000;
    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ff_check(avcodec_open2(context_.get(), codec, nullptr), "打开 AAC 编码器");
}
int MediaEncoder::drain(const PacketConsumer& consume) {
    while (true) {
        Packet packet = make_packet();
        const int result = metrics_->measure([&] { return avcodec_receive_packet(context_.get(), packet.get()); });
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
        ff_check(result, "接收编码包");
        metrics_->units.fetch_add(1, std::memory_order_relaxed);
        consume(std::move(packet));
    }
}
void MediaEncoder::send(const AVFrame* frame, const PacketConsumer& consume) {
    if (finished_) throw std::logic_error("编码器已排空");
    int result = metrics_->measure([&] { return avcodec_send_frame(context_.get(), frame); });
    if (result == AVERROR(EAGAIN)) {
        drain(consume);
        result = metrics_->measure([&] { return avcodec_send_frame(context_.get(), frame); });
    }
    ff_check(result, "发送编码帧");
    const int terminal = drain(consume);
    if (!frame) {
        if (terminal != AVERROR_EOF) throw std::runtime_error("编码器排空未到 EOF");
        finished_ = true;
    }
}
}
