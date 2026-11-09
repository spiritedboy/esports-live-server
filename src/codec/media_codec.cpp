/*
 * 初学者导读
 * 把 send/receive 看成有缓冲的状态机：send 提交输入，receive 尝试拿走全部已有输出。
 * EAGAIN 不是坏包：当前这一步暂时不能继续，要先在另一侧推进；EOF 才是排空终点。
 * 解码器参数来自 AVStream.codecpar，编码器参数则由节目输出规格决定。
 * FFmpeg 返回的编解码器描述符是借用指针；分配出的 context 才由 CodecContext 持有。
 * metrics 只包围 FFmpeg 调用；consume 还可能做缩放、排队、录制，不能混算进该耗时。
 * 本项目输出禁用 B 帧降低重排序延迟，但输入允许解码含 B 帧的视频。
 */
/*
 * 封装FFmpeg send/receive状态机。一次输入可能产生零个或多个输出，必须循环receive。
 * send借用输入对象；回调按值接收RAII对象并取得所有权；nullptr表示排空延迟数据。
 */
#include "codec/media_codec.h"
extern "C" {
#include <libavutil/opt.h>
}
namespace esports {
// 第一步选择解码实现并分配上下文，第二步复制 codecpar，最后 open2 建立可送包状态。
// stream 只用于读取参数，context_ 自己持有所需的副本。
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
    // 解码包时间单位来自AVStream；receive后的best_effort_timestamp按该输入时间基解释。
    context_->pkt_timebase = stream->time_base;
    context_->thread_count = 2;
    context_->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
    ff_check(avcodec_open2(context_.get(), codec, nullptr), "打开解码器");
}
// 一次 send 可能释放多帧，必须接收直到 EAGAIN/EOF。每次循环分配独立帧头，
// 这样回调把帧移到队列后，下一次 receive 不会覆盖已交付帧的数据。
int MediaDecoder::drain(const FrameConsumer& consume) {
    while (true) {
        Frame frame = make_frame();
        const int result = metrics_->measure([&] { return avcodec_receive_frame(context_.get(), frame.get()); });
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
        ff_check(result, "接收解码帧");
        if (frame->flags & AV_FRAME_FLAG_CORRUPT) throw std::runtime_error("损坏的解码帧");
        // 使用解码器估计的展示时间；含B帧时展示顺序和压缩包DTS顺序可能不同。
        frame->pts = frame->best_effort_timestamp;
        metrics_->units.fetch_add(1, std::memory_order_relaxed);
        consume(std::move(frame));
    }
}
// send不会接管调用者的Packet外壳。EAGAIN要求先接收已有输出，再重送同一个输入。
// nullptr进入排空，必须一直receive到EOF，之后本实例不再接受新包。
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
    // 编码PTS一格为1/90000秒；例如25fps相邻帧相差3600格，不能直接填帧序号。
    context_->time_base = AVRational{1, 90000};
    context_->framerate = AVRational{fps, 1};
    if (options.bitrate < 64000 || options.bitrate > 20000000 ||
        options.vbv_ms < 50 || options.vbv_ms > 2000 || options.gop_frames < 0)
        throw std::runtime_error("视频编码参数超出范围");
    context_->bit_rate = options.bitrate;
    context_->gop_size = options.gop_frames ? options.gop_frames : fps * 2;
    if (options.low_latency) {
        // VBV 将码率与允许缓存时长关联：1.2Mbit/s × 500ms = 600000bit。
        // 这控制编码码率突发，不保证网络和播放器总延迟小于 500ms。
        context_->rc_max_rate = options.bitrate;
        context_->rc_buffer_size = static_cast<int>(int64_t(options.bitrate) * options.vbv_ms / 1000);
        context_->rc_initial_buffer_occupancy = context_->rc_buffer_size / 2;
        context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ff_check(av_opt_set(context_->priv_data, "forced-idr", "1", 0), "启用强制 IDR");
        ff_check(av_opt_set(context_->priv_data, "x264-params",
            "scenecut=0:rc-lookahead=0:sync-lookahead=0", 0), "设置编码低延迟参数");
    }
    // 取消B帧减少重排序等待；GLOBAL_HEADER让封装层从extradata取得SPS/PPS等配置。
    context_->max_b_frames = 0;
    context_->thread_count = 2;
    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ff_check(av_opt_set(context_->priv_data, "preset", "veryfast", 0), "设置 x264 preset");
    ff_check(av_opt_set(context_->priv_data, "tune", "zerolatency", 0), "设置 x264 tune");
    ff_check(avcodec_open2(context_.get(), codec, nullptr), "打开 H.264 编码器");
}
// AAC 需要浮点平面 PCM；打开后 frame_size 才是实际要求的输入块长。
// PTS 以每声道采样数计，例如连续两块 1024 采样分别使用 0、1024。
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
// 每次 receive_packet 给出一包压缩数据；可能延迟到后续 send 或最终排空才有输出。
// 将 Packet 移给回调后当前变量为空，不会发生二次释放。
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
// 与解码方向相反：借用原始帧输入，移动交付压缩包输出。
// 音频编码有延迟，正常EOF必须send(nullptr)，否则末尾AAC包可能丢失。
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
