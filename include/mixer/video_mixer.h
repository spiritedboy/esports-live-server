/*
 * 滤镜图封装，由主处理线程独占；输入帧只借用，mix返回的新帧归调用者所有。
 */
#ifndef ESPORTS_MIXER_VIDEO_MIXER_H
#define ESPORTS_MIXER_VIDEO_MIXER_H
#include "codec/media_codec.h"
#include <vector>
extern "C" {
#include <libavfilter/avfilter.h>
}
namespace esports {
class VideoMixer {
public:
    VideoMixer() = default;
    ~VideoMixer() { avfilter_graph_free(&graph_); }
    VideoMixer(const VideoMixer&) = delete;
    VideoMixer& operator=(const VideoMixer&) = delete;
    void configure(const std::string& layout, int width, int height, int fps);
    Frame mix(const std::vector<const AVFrame*>& frames, int64_t tick);
private:
    AVFilterGraph* graph_ = nullptr;
    std::vector<AVFilterContext*> sources_;
    AVFilterContext* sink_ = nullptr;
};
}
#endif
