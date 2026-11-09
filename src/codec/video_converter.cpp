/*
 * 初学者导读
 * 注意两种性能路径：相同尺寸/格式时引用像素，不同尺寸/格式时实际缩放和转换。
 * 引用路径创建独立帧头，所以修改输出 PTS 不会改变源帧 PTS；像素仍可能是共享的。
 * 真正需要写共享像素时调用 av_frame_make_writable，FFmpeg 按需分离缓冲。
 * sws_scale 接收各平面的 data 与 linesize；linesize 可能含对齐填充，不等于图片宽度。
 * 32 是输出缓冲对齐要求，不是像素格式位数；实际缩放行数必须等于目标 height。
 */
/*
 * 视频格式适配：缩放并转为YUV420P；同尺寸同格式采用引用计数直通。
 */
#include "codec/video_converter.h"
#include <algorithm>
#include <cstdlib>
namespace esports {
Frame VideoConverter::convert(const AVFrame* source, int width, int height) {
    if (source->format == AV_PIX_FMT_YUV420P && source->width == width && source->height == height) {
        Frame output = make_frame();
        // 新建帧外壳并增加像素引用，不拷贝整幅图；source销毁后output仍有效。
        // 若后续要改共享像素，先av_frame_make_writable；改PTS等外壳字段无需复制像素。
        ff_check(av_frame_ref(output.get(), source), "引用同格式视频帧");
        output->sample_aspect_ratio = AVRational{1, 1};
        // 共享只读像素，但不把输入I/P/B类型作为重新编码决策。
        output->pict_type = AV_PICTURE_TYPE_NONE;
        return output;
    }
    // 尺寸或格式变化时 FFmpeg 会替换旧上下文；必须保存返回的新指针。
    context_ = sws_getCachedContext(context_, source->width, source->height,
        static_cast<AVPixelFormat>(source->format), width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!context_) throw std::runtime_error("创建视频缩放器失败");
    Frame output = make_frame();
    output->format = AV_PIX_FMT_YUV420P;
    output->width = width;
    output->height = height;
    output->sample_aspect_ratio = AVRational{1, 1};
    ff_check(av_frame_get_buffer(output.get(), 32), "分配视频帧");
    // 以整张源图为输入 slice，正确传入 stride；不能假定 Y/U/V 在内存里紧密连续。
    const int rows = sws_scale(context_, source->data, source->linesize, 0,
                              source->height, output->data, output->linesize);
    if (rows != height) throw std::runtime_error("视频缩放行数异常");
    return output;
}
}
