#include "input/stream_input.h"

namespace esports {
StreamInput::~StreamInput() { avformat_close_input(&context_); }

void StreamInput::open(const std::string& url, Logger& log) {
    if (context_) throw std::logic_error("输入已经打开");
    context_ = avformat_alloc_context();
    if (!context_) throw std::bad_alloc();
    context_->interrupt_callback = io_.callback();
    AVDictionary* options = nullptr;
    if (starts_with(url, "rtsp://")) {
        const int result = av_dict_set(&options, "rtsp_transport", "tcp", 0);
        if (result < 0) {
            av_dict_free(&options);
            ff_check(result, "设置 RTSP TCP 传输");
        }
    }
    io_.arm();
    const int result = avformat_open_input(&context_, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    io_.check(result, "打开输入");
    io_.arm();
    io_.check(avformat_find_stream_info(context_, nullptr), "探测输入流");
    for (unsigned i = 0; i < context_->nb_streams; ++i) {
        const AVStream* stream = context_->streams[i];
        log.log(LogLevel::Info, "输入流 " + std::to_string(i) + "，编码=" +
                avcodec_get_name(stream->codecpar->codec_id) + "，time_base=" +
                std::to_string(stream->time_base.num) + "/" +
                std::to_string(stream->time_base.den));
    }
}

int StreamInput::read(AVPacket* packet) {
    av_packet_unref(packet);
    io_.arm();
    const int result = av_read_frame(context_, packet);
    // 某些协议会把中断映射为 EOF；超时/用户停止必须先于正常 EOF 判断。
    if (result < 0 && (io_.stopped() || io_.expired())) io_.check(result, "读取输入包");
    if (result != AVERROR_EOF && result != AVERROR(EAGAIN)) io_.check(result, "读取输入包");
    return result;
}
} // namespace esports
