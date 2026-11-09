/*
 * 初学者导读
 * configure 创建滤镜图，mix 向每个 buffer 输入一张图，再从 buffersink 取节目画面。
 * split 左右拼接两路，pip 把第二路缩小覆盖第一路，grid 将四路排成 2×2。
 * 输入已经由 VideoConverter 统一为目标尺寸 YUV420P；混合时再缩放为各布局中的子窗口。
 * tick 是节目视频帧序号，例如 25fps 下 tick=25 表示 1 秒，不是 25 微秒。
 * 每次 mix 都新建帧引用并重设该引用的 PTS；不会修改主线程保留的源帧外壳。
 * graph_ 拥有所有滤镜节点；sources_/sink_ 只是图内节点的借用指针，不单独释放。
 * 切换布局重新 configure 会释放旧图，因此不能让其他线程同时使用旧节点。
 */
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
    // 创建或替换指定布局的滤镜图；width/height/fps 要与上游转换和节目配置一致。
    void configure(const std::string& layout, int width, int height, int fps);
    // 借用每路当前画面并为图创建独立引用；返回同一 tick 的新节目帧。
    Frame mix(const std::vector<const AVFrame*>& frames, int64_t tick);
private:
    // 图拥有节点；sources_、sink_ 只是导航指针，图释放后立刻失效。
    AVFilterGraph* graph_ = nullptr;
    std::vector<AVFilterContext*> sources_;
    AVFilterContext* sink_ = nullptr;
};
}
#endif
