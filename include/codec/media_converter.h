/*
 * 单线程格式转换器。返回的Frame交给调用者持有，传入指针仅借用。
 * AudioConverter借用已打开的目标编码器，编码器必须晚于转换器销毁。
 */
#ifndef ESPORTS_CODEC_MEDIA_CONVERTER_H
#define ESPORTS_CODEC_MEDIA_CONVERTER_H
#include "codec/media_codec.h"
extern "C" {
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
}
namespace esports {
class VideoConverter {
public:
    ~VideoConverter() { sws_freeContext(context_); }
    VideoConverter() = default;
    VideoConverter(const VideoConverter&) = delete;
    VideoConverter& operator=(const VideoConverter&) = delete;
    Frame convert(const AVFrame* source, int width, int height);
private:
    SwsContext* context_ = nullptr;
};
class AudioConverter {
public:
    explicit AudioConverter(AVCodecContext* target) : target_(target) {}
    ~AudioConverter();
    AudioConverter(const AudioConverter&) = delete;
    AudioConverter& operator=(const AudioConverter&) = delete;
    void send(const AVFrame* source, int64_t pts_samples, const FrameConsumer& consume);
    void finish(const FrameConsumer& consume);
private:
    Frame allocate(int samples);
    void append(const AVFrame* frame, const FrameConsumer& consume);
    void drain(bool final, const FrameConsumer& consume);
    AVCodecContext* target_;
    SwrContext* swr_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;
    AVChannelLayout input_layout_ = {};
    int input_rate_ = 0;
    int input_format_ = -1;
    int64_t next_pts_ = 0;
    int64_t expected_input_pts_ = 0;
    bool initialized_ = false;
};
}
#endif
