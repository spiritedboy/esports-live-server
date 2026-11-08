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
    Frame convert(const AVFrame* source, int width, int height);
private:
    SwsContext* context_ = nullptr;
};
}
#endif
