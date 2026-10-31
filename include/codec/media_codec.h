/*
 * 媒体对象所有权与编解码接口：Frame/Packet独占对象外壳，像素/压缩缓冲按FFmpeg规则引用计数。
 * 同一个编解码器不能被多个调用线程并发send；FFmpeg内部线程由其自身管理。
 */
#ifndef ESPORTS_CODEC_MEDIA_CODEC_H
#define ESPORTS_CODEC_MEDIA_CODEC_H
#include "common/ffmpeg.h"
#include <functional>
#include "monitor/metrics.h"
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace esports {
struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
typedef std::unique_ptr<AVFrame, FrameDeleter> Frame;
inline Frame make_frame() {
    Frame f(av_frame_alloc());
    if (!f) throw std::bad_alloc();
    return f;
}
struct CodecDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
typedef std::unique_ptr<AVCodecContext, CodecDeleter> CodecContext;
// 回调按值接收unique_ptr：调用consume(std::move(frame))后，原frame不再拥有该帧。
// get()只借出裸指针，绝不能另建一个unique_ptr接管它。
typedef std::function<void(Frame)> FrameConsumer;
typedef std::function<void(Packet)> PacketConsumer;

class MediaDecoder {
public:
    explicit MediaDecoder(CodecMetrics* metrics = nullptr) : metrics_(metrics ? metrics : &owned_metrics_) {}
    void open(const AVStream* stream);
    void send(const AVPacket* packet, const FrameConsumer& consume);
    // 借用指针，不转移所有权。
    AVCodecContext* context() const { return context_.get(); }
private:
    int drain(const FrameConsumer& consume);
    CodecMetrics owned_metrics_;
    CodecMetrics* metrics_ = &owned_metrics_;
    CodecContext context_;
    bool finished_ = false;
};
struct VideoEncodingOptions {
    bool low_latency = false;
    int bitrate = 1200000;
    int gop_frames = 0;
    int vbv_ms = 500;
};
class MediaEncoder {
public:
    const CodecMetrics& metrics() const { return owned_metrics_; }
    void open_video(int width, int height, int fps, const VideoEncodingOptions& options = VideoEncodingOptions());
    void open_audio();
    void send(const AVFrame* frame, const PacketConsumer& consume);
    // 借用指针，不转移所有权。
    AVCodecContext* context() const { return context_.get(); }
private:
    int drain(const PacketConsumer& consume);
    CodecMetrics owned_metrics_;
    CodecMetrics* metrics_ = &owned_metrics_;
    CodecContext context_;
    bool finished_ = false;
};
}
#endif
