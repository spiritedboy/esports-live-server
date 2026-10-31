// 用RLIMIT_FSIZE触发录制EFBIG，同时通过管道保留直播支路；分流前摘要用于检查同次运行的数据完整性。
#include "recording/recorder.h"
#include "codec/media_codec.h"
#include "mixer/audio_mixer.h"
#include <iostream>
#include <sstream>
#include <iomanip>
extern "C" {
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}
#include <sys/resource.h>
namespace {
// 实际FLV封装到stdout管道：管道不受RLIMIT_FSIZE影响，录制普通文件受限。
struct PipeOutput {
    AVFormatContext* context = nullptr;
    ~PipeOutput() {
        if (context) { if (context->pb) avio_closep(&context->pb); avformat_free_context(context); }
    }
    void open(const std::vector<AVCodecContext*>& codecs) {
        esports::ff_check(avformat_alloc_output_context2(&context, nullptr, "flv", nullptr), "创建测试FLV");
        if (!context) throw std::bad_alloc();
        context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
        for (auto* codec : codecs) {
            AVStream* stream = avformat_new_stream(context, nullptr);
            if (!stream) throw std::bad_alloc();
            esports::ff_check(avcodec_parameters_from_context(stream->codecpar, codec), "复制测试参数");
            stream->time_base = codec->time_base;
        }
        esports::ff_check(avio_open(&context->pb, "pipe:1", AVIO_FLAG_WRITE), "打开测试管道");
        esports::ff_check(avformat_write_header(context, nullptr), "测试写头");
    }
    void write(AVPacket* packet, AVRational tb) {
        av_packet_rescale_ts(packet, tb, context->streams[packet->stream_index]->time_base);
        packet->pos = -1;
        esports::ff_check(av_interleaved_write_frame(context, packet), "管道输出包");
    }
    void finish() {
        esports::ff_check(av_write_trailer(context), "管道写尾");
        avio_flush(context->pb);
        esports::ff_check(context->pb->error, "管道刷新");
    }
};
}
int main(int argc, char** argv) {
    using namespace esports;
    if (argc != 3) return 2;
    std::signal(SIGPIPE, SIG_IGN);
    // 让内核通过write返回EFBIG，以验证FFmpeg错误传播和支路隔离。
    std::signal(SIGXFSZ, SIG_IGN);
    try {
        const int limit = std::stoi(argv[2]);
        if (limit) {
            struct rlimit resource = {static_cast<rlim_t>(limit), static_cast<rlim_t>(limit)};
            if (setrlimit(RLIMIT_FSIZE, &resource)) throw std::runtime_error("无法设置测试文件上限");
        }
        Logger log(std::cerr);
        MediaEncoder video, audio;
        VideoEncodingOptions options; options.low_latency = true; options.gop_frames = 25;
        video.open_video(160, 120, 25, options); audio.open_audio();
        RecordingOptions recording; recording.prefix = argv[1];
        Recorder recorder(recording, log);
        recorder.open({video.context(), audio.context()});
        PipeOutput output; output.open({video.context(), audio.context()});
        int video_packets = 0, audio_packets = 0;
        std::vector<std::string> references;
        std::unique_ptr<AVSHA, void(*)(void*)> sha(av_sha_alloc(), av_free);
        if (!sha) throw std::bad_alloc();
        const PacketConsumer vp = [&](Packet packet) {
            ff_check(av_sha_init(sha.get(), 256), "初始化包摘要");
            // libx264输出Annex B，FLV将起始码改为4字节NAL长度。
            // 先规范为相同AVCC表示再比较摘要，不能把封装转换误判为内容损坏。
            const auto start_code = [&](int at) {
                if (at+3 <= packet->size && packet->data[at]==0 && packet->data[at+1]==0) {
                    if (packet->data[at+2]==1) return 3;
                    if (at+4 <= packet->size && packet->data[at+2]==0 && packet->data[at+3]==1) return 4;
                }
                return 0;
            };
            if (!start_code(0)) throw std::runtime_error("测试编码器未输出预期Annex B");
            int at = 0;
            while (at < packet->size) {
                const int begin = at + start_code(at);
                int end = begin;
                while (end < packet->size && !start_code(end)) ++end;
                const uint32_t size = static_cast<uint32_t>(end-begin);
                const uint8_t length[4] = {static_cast<uint8_t>(size>>24), static_cast<uint8_t>(size>>16),
                    static_cast<uint8_t>(size>>8), static_cast<uint8_t>(size)};
                av_sha_update(sha.get(), length, 4);
                av_sha_update(sha.get(), packet->data+begin, size);
                at = end;
            }
            uint8_t digest[32]; av_sha_final(sha.get(), digest);
            std::ostringstream hash; hash << "SHA256:" << std::hex << std::setfill('0');
            for (auto byte : digest) hash << std::setw(2) << static_cast<unsigned>(byte);
            references.push_back(hash.str());
            packet->stream_index = 0;
            recorder.submit(packet.get(), video.context()->time_base);
            output.write(packet.get(), video.context()->time_base); ++video_packets;
        };
        const PacketConsumer ap = [&](Packet packet) {
            packet->stream_index = 1;
            recorder.submit(packet.get(), audio.context()->time_base);
            output.write(packet.get(), audio.context()->time_base); ++audio_packets;
        };
        int64_t cursor = 0;
        for (int tick = 0; tick < 100; ++tick) {
            Frame frame = make_frame();
            frame->format = AV_PIX_FMT_YUV420P; frame->width = 160; frame->height = 120;
            ff_check(av_frame_get_buffer(frame.get(), 32), "生成测试帧");
            for (int plane = 0; plane < 3; ++plane) {
                const int width = plane ? 80 : 160, height = plane ? 60 : 120;
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                        frame->data[plane][y*frame->linesize[plane]+x] = plane ? 128 : 16+(x*7+y*3+tick*11)%220;
            }
            frame->pts = tick*3600;
            video.send(frame.get(), vp);
            while (cursor < (tick+1)*1920) {
                Frame block = audio_frame(1024, cursor);
                audio.send(block.get(), ap); cursor += 1024;
            }
        }
        video.send(nullptr, vp); audio.send(nullptr, ap);
        recorder.finish(); output.finish();
        if (video_packets != 100 || audio_packets < 180) throw std::runtime_error("主支路未完整输出");
        for (const auto& hash : references) std::cerr << "SHA256REF=" << hash << '\n';
        std::cerr << "主支路完整：视频=" << video_packets << "，音频=" << audio_packets << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
