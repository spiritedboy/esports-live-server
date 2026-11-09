/*
 * 初学者导读
 * 先区分三个对象：AVStream 是轨道描述，AVCodecContext 是有内部状态的编解码器，
 * AVFrame 是未压缩的数据。视频 Frame 装像素，音频 Frame 装 PCM 采样。
 * make_frame 只分配帧外壳；自行生成像素/PCM 时还需要 av_frame_get_buffer。
 * Frame 的唯一所有权不代表像素只能被一个帧引用：av_frame_ref 可以共享底层缓冲。
 * 解码方向：StreamInput 的 Packet → MediaDecoder → FrameConsumer。
 * 编码方向：合成/混音 Frame → MediaEncoder → PacketConsumer → 输出与录制。
 * Consumer 在调用 send 的同一个线程同步执行，不是自动启动的新线程。
 * 一份输入未必立即对应一份输出，所以 send 内部必须循环 drain。
 * nullptr 输入表示流结束后的排空，不是“暂时没有数据”；排空后不能继续送新输入。
 */
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
// av_frame_free 同时减少像素/PCM 缓冲引用并释放外壳。共享缓冲在最后一个引用释放时销毁。
struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
typedef std::unique_ptr<AVFrame, FrameDeleter> Frame;
inline Frame make_frame() {
    Frame f(av_frame_alloc());
    if (!f) throw std::bad_alloc();
    return f;
}
// 上下文拥有内部线程、参考帧与编码缓存，用 avcodec_free_context 成套释放。
struct CodecDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
typedef std::unique_ptr<AVCodecContext, CodecDeleter> CodecContext;
// 回调按值接收unique_ptr：调用consume(std::move(frame))后，原frame不再拥有该帧。
// get()只借出裸指针，绝不能另建一个unique_ptr接管它。
typedef std::function<void(Frame)> FrameConsumer;
typedef std::function<void(Packet)> PacketConsumer;

class MediaDecoder {
public:
    explicit MediaDecoder(CodecMetrics* metrics = nullptr) : metrics_(metrics ? metrics : &owned_metrics_) {}
    // 借用轨道描述并复制参数；成功后可 send 压缩包。每个实例只打开一次。
    void open(const AVStream* stream);
    // packet 的时间基来自 open 时的轨道；回调可收到零到多帧，并取得每帧所有权。
    // packet=nullptr 结束输入并排空。consume 抛出的异常会沿当前调用栈传播。
    void send(const AVPacket* packet, const FrameConsumer& consume);
    // 借用指针，不转移所有权。
    AVCodecContext* context() const { return context_.get(); }
private:
    // 内部循环接收直到 EAGAIN（需更多输入）或 EOF（排空完成），返回该终止状态。
    int drain(const FrameConsumer& consume);
    CodecMetrics owned_metrics_;
    CodecMetrics* metrics_ = &owned_metrics_;
    CodecContext context_;
    bool finished_ = false;
};
// 码率单位 bit/s；gop_frames 是两关键帧间隔目标，0 使用默认；vbv_ms 控制码控缓冲时长。
struct VideoEncodingOptions {
    bool low_latency = false;
    int bitrate = 1200000;
    int gop_frames = 0;
    int vbv_ms = 500;
};
class MediaEncoder {
public:
    const CodecMetrics& metrics() const { return owned_metrics_; }
    // 创建 libx264 视频编码器；输入需是相同尺寸的 YUV420P，PTS 使用编码器 time_base。
    void open_video(int width, int height, int fps, const VideoEncodingOptions& options = VideoEncodingOptions());
    // 创建 48kHz 双声道 FLTP 的 AAC 编码器；块大小应读取打开后的 context()->frame_size。
    void open_audio();
    // 只借用输入帧；FFmpeg 若要延后使用数据会持有自己的引用。
    // 回调输出包的时间基为 context()->time_base，尚未换成容器时间基。
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
