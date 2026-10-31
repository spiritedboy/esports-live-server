// 以真实编码和封装配合可替换PacketSink注入输出故障；不能代替真实RTMP接收播放验收。
#include "output/async_output.h"
#include "codec/media_codec.h"
#include "mixer/audio_mixer.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
extern "C" {
#include <libavutil/imgutils.h>
}
namespace {
struct FaultState {
    std::atomic<int> opens{0}, violations{0};
    std::string directory;
};
class FaultSink : public esports::PacketSink {
public:
    FaultSink(esports::IoControl& io, FaultState& state)
        : output_(io, true), state_(state), id_(state.opens.fetch_add(1)) {}
    void open(const std::string&, AVFormatContext* streams, esports::Logger& log) override {
        output_.open(state_.directory + "/session-" + std::to_string(id_) + ".flv", streams, log);
        if (id_ == 0) std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    void write(AVPacket* packet, AVRational tb, int64_t origin) override {
        if (!writes_ && (packet->stream_index != 0 || !(packet->flags & AV_PKT_FLAG_KEY) ||
            av_rescale_q(packet->dts, tb, AVRational{1, AV_TIME_BASE}) != origin)) {
            ++state_.violations;
            throw std::runtime_error("恢复会话没有从新 IDR/共同原点开始");
        }
        if (id_ == 0 && writes_ == 12) {
            av_packet_unref(packet); // 模拟 FFmpeg 写失败后已经消耗包引用。
            throw std::runtime_error("注入：输出连接中断");
        }
        if (id_ == 1 && writes_ == 8) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        output_.write(packet, tb, origin);
        ++writes_;
    }
    void finish() override { output_.finish(); }
private:
    esports::StreamOutput output_;
    FaultState& state_;
    int id_, writes_ = 0;
};
class RefuseSink : public esports::PacketSink {
public:
    void open(const std::string&, AVFormatContext*, esports::Logger&) override {
        throw std::runtime_error("注入：连接失败");
    }
    void write(AVPacket*, AVRational, int64_t) override { throw std::logic_error("不可达"); }
    void finish() override {}
};
}
int main(int argc, char** argv) {
    using namespace esports;
    if (argc != 2) return 2;
    try {
        NetworkRuntime network;
        Logger log(std::cerr);
        MediaEncoder video, audio;
        VideoEncodingOptions encoding;
        encoding.low_latency = true; encoding.gop_frames = 25; encoding.bitrate = 200000;
        video.open_video(160, 120, 25, encoding);
        audio.open_audio();
        OutputOptions options;
        options.queue.packets = 20;
        options.queue.bytes = 65536;
        options.queue.latency_ms = 80;
        options.backoff_ms = 10;
        options.reconnect_attempts = 5;
        FaultState faults; faults.directory = argv[1];
        AsyncOutput output(options, true, log, [&](IoControl& io) {
            return std::unique_ptr<PacketSink>(new FaultSink(io, faults));
        });
        output.open("rtmp://test-only/live", {video.context(), audio.context()});
        const PacketConsumer vp = [&](Packet p) { p->stream_index = 0; output.submit(std::move(p), video.context()->time_base); };
        const PacketConsumer ap = [&](Packet p) { p->stream_index = 1; output.submit(std::move(p), audio.context()->time_base); };
        int64_t cursor = 0;
        for (int tick = 0; tick < 250; ++tick) {
            Frame frame = make_frame();
            frame->format = AV_PIX_FMT_YUV420P;
            frame->width = 160; frame->height = 120;
            ff_check(av_frame_get_buffer(frame.get(), 32), "生成恢复测试视频");
            ptrdiff_t lines[4] = {frame->linesize[0], frame->linesize[1], frame->linesize[2], 0};
            ff_check(av_image_fill_black(frame->data, lines, AV_PIX_FMT_YUV420P, AVCOL_RANGE_MPEG, 160, 120),
                     "生成测试像素");
            frame->pts = tick * 3600;
            if (output.take_keyframe_request()) frame->pict_type = AV_PICTURE_TYPE_I;
            video.send(frame.get(), vp);
            while (cursor < (tick + 1) * 1920) {
                Frame block = audio_frame(1024, cursor);
                audio.send(block.get(), ap);
                cursor += 1024;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        video.send(nullptr, vp); audio.send(nullptr, ap);
        output.finish();
        const auto stats = output.snapshot();
        if (faults.violations || stats.sessions < 2 || !stats.failures || !stats.retries ||
            stats.video_packets < 20 || !stats.queue.dropped_video || !stats.queue.dropped_audio ||
            stats.queue.peak_packets > 20 || stats.queue.peak_bytes > 65536)
            throw std::runtime_error("恢复统计/界限检查失败");
        std::cout << "会话=" << stats.sessions << "，重试=" << stats.retries
                  << "，成功视频=" << stats.video_packets << "，成功音频=" << stats.audio_packets
                  << "，丢视频=" << stats.queue.dropped_video << "，丢音频=" << stats.queue.dropped_audio
                  << "，队列峰值=" << stats.queue.peak_packets << "/" << stats.queue.peak_bytes
                  << "，排队峰值us=" << stats.max_queue_us << "，写入峰值us=" << stats.max_write_us << '\n';

        // 不依赖网络的退避取消和 EOF 未恢复验证。
        OutputOptions failing;
        failing.backoff_ms = 4000;
        failing.reconnect_attempts = 2;
        const SinkFactory refuse = [](IoControl&) { return std::unique_ptr<PacketSink>(new RefuseSink); };
        AsyncOutput cancelled(failing, true, log, refuse);
        cancelled.open("rtmp://test-only/live", {video.context(), audio.context()});
        Packet key = make_packet();
        ff_check(av_new_packet(key.get(), 8), "生成取消测试包");
        key->pts = key->dts = 0; key->stream_index = 0; key->flags = AV_PKT_FLAG_KEY;
        cancelled.submit(std::move(key), video.context()->time_base);
        const int64_t deadline = av_gettime_relative() + 2000000;
        while (!cancelled.snapshot().failures && av_gettime_relative() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!cancelled.snapshot().failures) throw std::runtime_error("故障注入未触发");
        const int64_t start = av_gettime_relative();
        cancelled.cancel(); cancelled.finish();
        if (av_gettime_relative() - start > 500000) throw std::runtime_error("退避取消过慢");

        failing.backoff_ms = 10;
        AsyncOutput eof(failing, true, log, refuse);
        eof.open("rtmp://test-only/live", {video.context(), audio.context()});
        key = make_packet();
        ff_check(av_new_packet(key.get(), 8), "生成EOF测试包");
        key->pts = key->dts = 0; key->stream_index = 0; key->flags = AV_PKT_FLAG_KEY;
        eof.submit(std::move(key), video.context()->time_base);
        bool failed = false;
        try { eof.finish(); } catch (const std::runtime_error&) { failed = true; }
        if (!failed) throw std::runtime_error("EOF未恢复被误报成功");
        std::cout << "退避取消、EOF未恢复报错通过（测试传输，不是RTMP成功会话）\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
