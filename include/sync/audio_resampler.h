/*
 * 直播专用重采样与时钟补偿器，输入pts_samples和输出PTS都按1/48000计时。
 * 送入的AVFrame仅借用，输出Frame经回调移动交付。
 */
#ifndef ESPORTS_SYNC_AUDIO_RESAMPLER_H
#define ESPORTS_SYNC_AUDIO_RESAMPLER_H
#include "codec/media_codec.h"
#include "sync/av_synchronizer.h"
extern "C" {
#include <libswresample/swresample.h>
}
namespace esports {
struct AudioSyncStats {
    int64_t max_error = 0, corrections = 0, gaps = 0, resets = 0, missing = 0;
};
class AudioResampler {
public:
    AudioResampler() = default;
    ~AudioResampler();
    AudioResampler(const AudioResampler&) = delete;
    AudioResampler& operator=(const AudioResampler&) = delete;
    void send(const AVFrame*, int64_t pts_samples, const FrameConsumer&);
    void finish(const FrameConsumer&);
    AudioSyncStats stats() const;
private:
    SwrContext* context_ = nullptr;
    AVChannelLayout layout_ = {};
    int rate_ = 0, format_ = -1;
    int64_t next_ = 0;
    SourceClock clock_{48000};
    AudioSyncStats stats_;
};
}
#endif
