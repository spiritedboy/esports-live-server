/*
 * 单路线程内转码链路：解码回调 → 格式转换回调 → 编码回调 → 封装。
 * 这些回调是同步函数调用，并非各自启动线程；EOF按数据流方向逐层排空。
 */
#include "manager/transcoder.h"
#include "input/stream_input.h"
#include "output/stream_output.h"
#include "codec/media_converter.h"
#include <algorithm>
#include <chrono>
#include <thread>
namespace esports {
int run_transcode(const TranscodeOptions& options, const volatile std::sig_atomic_t& stop, Logger& log) {
    NetworkRuntime network;
    IoControl io(stop, options.timeout_ms);
    StreamInput input(io);
    StreamOutput output(io);
    MediaDecoder video_decoder, audio_decoder;
    MediaEncoder video_encoder, audio_encoder;
    VideoConverter video_converter;
    std::unique_ptr<AudioConverter> audio_converter;
    int video_index = -1, audio_index = -1, video_output = -1, audio_output = -1;
    int64_t videos = 0, audios = 0;
    try {
        input.open(options.input, log);
        AVFormatContext* format = input.context();
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            AVStream* stream = format->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (video_index >= 0) throw std::runtime_error("转码仅支持一路视频");
                video_index = i;
                video_decoder.open(stream);
            } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                if (audio_index >= 0) throw std::runtime_error("转码仅支持一路音频");
                audio_index = i;
                audio_decoder.open(stream);
            }
        }
        std::vector<AVCodecContext*> encoders;
        if (video_index >= 0) {
            video_encoder.open_video(options.width, options.height, options.fps);
            video_output = encoders.size();
            encoders.push_back(video_encoder.context());
        }
        if (audio_index >= 0) {
            audio_encoder.open_audio();
            audio_output = encoders.size();
            encoders.push_back(audio_encoder.context());
            audio_converter.reset(new AudioConverter(audio_encoder.context()));
        }
        if (encoders.empty()) throw std::runtime_error("没有可转码的音视频流");
        output.open_encoded(options.output, encoders, log);
        int64_t origin = format->start_time;
        int64_t pacing_origin = AV_NOPTS_VALUE;
        int64_t wall_origin = 0;
        int64_t previous_video = AV_NOPTS_VALUE;
        // 所有解码帧先换算成共同原点下的微秒；视频/音频随后各转到自己的编码time_base。
        const auto normalized = [&](const AVFrame* frame, int index) {
            if (frame->pts == AV_NOPTS_VALUE) throw std::runtime_error("解码帧缺少 PTS");
            const int64_t us = av_rescale_q(frame->pts, format->streams[index]->time_base,
                                           AVRational{1, AV_TIME_BASE});
            if (us < -(int64_t(1) << 55) || us > (int64_t(1) << 55))
                throw std::runtime_error("帧时间戳超出范围");
            if (origin == AV_NOPTS_VALUE) origin = us;
            if (origin < -(int64_t(1) << 55) || origin > (int64_t(1) << 55))
                throw std::runtime_error("输入时间原点超出范围");
            const int64_t pts = us - origin;
            if (options.realtime) {
                if (pacing_origin == AV_NOPTS_VALUE) {
                    pacing_origin = pts;
                    wall_origin = av_gettime_relative();
                }
                while (!io.stopped()) {
                    const int64_t delay = pts - pacing_origin - (av_gettime_relative() - wall_origin);
                    if (delay <= 0) break;
                    std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(delay, 10000)));
                }
            }
            return pts;
        };
        // 回调从下游向上游组装：先定义包输出，再定义编码输入，最后接到解码器。
        // 这些引用捕获只在当前函数存活期间同步调用。
        const PacketConsumer video_packet = [&](Packet packet) {
            packet->stream_index = video_output;
            output.write(packet.get(), video_encoder.context()->time_base, 0);
        };
        const PacketConsumer audio_packet = [&](Packet packet) {
            packet->stream_index = audio_output;
            output.write(packet.get(), audio_encoder.context()->time_base, 0);
        };
        const FrameConsumer audio_ready = [&](Frame frame) {
            audio_encoder.send(frame.get(), audio_packet);
            ++audios;
        };
        const FrameConsumer video_frame = [&](Frame frame) {
            const int64_t us = normalized(frame.get(), video_index);
            Frame converted = video_converter.convert(frame.get(), options.width, options.height);
            converted->pts = av_rescale_q(us, AVRational{1, AV_TIME_BASE},
                                          video_encoder.context()->time_base);
            if (previous_video != AV_NOPTS_VALUE && converted->pts <= previous_video)
                throw std::runtime_error("视频帧 PTS 不递增");
            previous_video = converted->pts;
            video_encoder.send(converted.get(), video_packet);
            ++videos;
        };
        const FrameConsumer audio_frame = [&](Frame frame) {
            const int64_t us = normalized(frame.get(), audio_index);
            const int64_t samples = av_rescale_q(us, AVRational{1, AV_TIME_BASE},
                                                audio_encoder.context()->time_base);
            audio_converter->send(frame.get(), samples, audio_ready);
        };
        Packet packet = make_packet();
        int64_t progress = av_gettime_relative();
        while (!io.stopped()) {
            const int result = input.read(packet.get());
            if (result == AVERROR_EOF) {
                if (is_network_input(options.input)) throw std::runtime_error("网络输入结束或断流");
                break;
            }
            if (result == AVERROR(EAGAIN)) {
                if (av_gettime_relative() - progress > int64_t(options.timeout_ms) * 1000)
                    throw std::runtime_error("转码读取超时");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            progress = av_gettime_relative();
            if (packet->flags & AV_PKT_FLAG_CORRUPT) throw std::runtime_error("输入包损坏");
            if (packet->stream_index == video_index) video_decoder.send(packet.get(), video_frame);
            else if (packet->stream_index == audio_index) audio_decoder.send(packet.get(), audio_frame);
        }
        // 用户停止不继续产生新媒体；正常 EOF 必须排空所有延迟级。
        if (!io.stopped()) {
            if (video_index >= 0) {
                video_decoder.send(nullptr, video_frame);
                video_encoder.send(nullptr, video_packet);
            }
            if (audio_index >= 0) {
                audio_decoder.send(nullptr, audio_frame);
                // 先排出重采样延迟和FIFO剩余PCM，再排空AAC编码器；倒过来会丢失最后的声音。
                audio_converter->finish(audio_ready);
                audio_encoder.send(nullptr, audio_packet);
            }
        }
        output.finish();
    } catch (const std::exception&) {
        try { output.finish(); }
        catch (const std::exception& e) { log.log(LogLevel::Warning, e.what()); }
        if (!io.stopped()) throw;
        return 130;
    }
    log.log(LogLevel::Info, "转码结束：视频帧=" + std::to_string(videos) +
            "，AAC 输入帧=" + std::to_string(audios));
    if (stop) return 130;
    if (!videos && !audios) throw std::runtime_error("没有生成媒体帧");
    return 0;
}
}
