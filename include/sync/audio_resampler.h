/*
 * 初学者导读
 * 调用路径：AudioSource 解码回调 → send → SwrContext 转换 → consume → AudioQueue。
 * pts_samples 是调用方已换算到 48kHz 的起点；frame 本身的输入采样率可以不同。
 * 输出统一为 48kHz/双声道/FLTP，输出 PTS 是每声道采样位置，不是毫秒。
 * next_ 表示下一个输出块起点；内部重采样延迟也参与误差估计，不能忽略。
 * layout_/rate_/format_ 记录首次输入的格式，运行中格式变更会报错，而非悄悄解释错数据。
 * finish 只排空重采样器内部尾部采样；调用者必须先排空解码器，再调用它。
 * stats 的 max_error 单位是采样，corrections/gaps/resets/missing 是事件次数。
 */
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
    // frame 只在本次调用中借用；pts_samples 单独指定统一时间起点，consume 接管输出 Frame。
    void send(const AVFrame*, int64_t pts_samples, const FrameConsumer&);
    // 正常 EOF 后调用，循环取出 Swr 尚未输出的尾部采样；未初始化时直接返回。
    void finish(const FrameConsumer&);
    AudioSyncStats stats() const;
private:
    // 持有重采样状态；layout_ 是深拷贝的声道布局，析构时必须分别释放二者。
    SwrContext* context_ = nullptr;
    AVChannelLayout layout_ = {};
    int rate_ = 0, format_ = -1;
    int64_t next_ = 0;
    SourceClock clock_{48000};
    AudioSyncStats stats_;
};
}
#endif
