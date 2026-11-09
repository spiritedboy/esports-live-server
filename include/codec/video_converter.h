/*
 * 初学者导读
 * 调用者是 VideoSource 工作线程；输出会交给 FrameQueue，再供主线程合成。
 * convert 的 width/height 是目标尺寸；source 必须是有效的视频帧且在调用期间存活。
 * 目标固定为 YUV420P：Y 独立一平面，U/V 的宽和高各为亮度的一半。
 * 同格式同尺寸时仅引用像素；需要缩放或变换格式时分配新缓冲并执行 sws_scale。
 * 本类缓存 SwsContext 避免每帧重建；因此同一个实例不能并发 convert。
 * 返回值拥有新外壳；节目时间戳由 VideoSource 另外设置，不能假定两条路径都复制 PTS。
 */
/*
 * 单线程格式转换器。返回的Frame交给调用者持有，传入指针仅借用。
 */
#ifndef ESPORTS_CODEC_VIDEO_CONVERTER_H
#define ESPORTS_CODEC_VIDEO_CONVERTER_H
#include "codec/media_codec.h"
extern "C" {
#include <libswscale/swscale.h>
}
namespace esports {
class VideoConverter {
public:
    ~VideoConverter() { sws_freeContext(context_); }
    VideoConverter() = default;
    VideoConverter(const VideoConverter&) = delete;
    VideoConverter& operator=(const VideoConverter&) = delete;
    // source 只借用，返回值由调用者拥有。两种转换路径都保证输出尺寸和 YUV420P 格式。
    Frame convert(const AVFrame* source, int width, int height);
private:
    // 持有可复用的缩放状态；sws_getCachedContext 可能重建它，析构时统一释放。
    SwsContext* context_ = nullptr;
};
}
#endif
