/*
 * 初学者导读
 * 滤镜图像一条有名字的管线：[in0]/[in1] 接 buffer，处理中间标签连 scale/stack，
 * 最终 [out] 接 buffersink。AVFilterInOut 链表用于把字符串里的名字接到已有节点。
 * configure 会替换整个图；parse_ptr 会修改端点链表，所以异常时释放当前链表头。
 * 输入 time_base 声明为 1/fps，mix 传 tick；不要把微秒 PTS 直接交给这个滤镜图。
 * 每路新建 AVFrame 引用再送图，保护主线程还要复用的 current 源帧。
 * 图输出暂时仍是 tick 单位，run_live 会在送编码器前换成 1/90000。
 */
/*
 * 合成器只处理同一节目时刻的帧；选帧和缺失源处理由上层调度完成。
 * 滤镜图路径为buffer → scale/stack/overlay → buffersink，图及子节点归本类持有。
 */
#include "mixer/video_mixer.h"
#include <sstream>
extern "C" {
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/mem.h>
}
namespace esports {
void VideoMixer::configure(const std::string& layout, int width, int height, int fps) {
    if (layout != "split" && layout != "grid" && layout != "pip")
        throw std::runtime_error("未知布局");
    avfilter_graph_free(&graph_);
    sources_.clear();
    sink_ = nullptr;
    graph_ = avfilter_graph_alloc();
    if (!graph_) throw std::bad_alloc();
    graph_->nb_threads = 2;
    const int count = layout == "grid" ? 4 : 2;
    AVFilterInOut* raw_outputs = nullptr;
    AVFilterInOut* raw_inputs = nullptr;
    // parse_ptr 会改变链表头；所有正常/失败分支都按当前头释放。
    try {
        for (int i = 0; i < count; ++i) {
            const std::string name = "in" + std::to_string(i);
            // buffer声明的time_base=1/fps，所以mix注入的是节目tick，不能仍用源微秒PTS。
            const std::string parameters = "video_size=" + std::to_string(width) + "x" +
                std::to_string(height) + ":pix_fmt=" + std::to_string(AV_PIX_FMT_YUV420P) +
                ":time_base=1/" + std::to_string(fps) + ":pixel_aspect=1/1";
            AVFilterContext* source = nullptr;
            ff_check(avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"),
                name.c_str(), parameters.c_str(), nullptr, graph_), "创建视频 buffer");
            sources_.push_back(source);
            AVFilterInOut* end = avfilter_inout_alloc();
            if (!end) throw std::bad_alloc();
            end->next = raw_outputs;
            raw_outputs = end;
            end->name = av_strdup(name.c_str());
            if (!end->name) throw std::bad_alloc();
            end->filter_ctx = source;
            end->pad_idx = 0;
        }
        ff_check(avfilter_graph_create_filter(&sink_, avfilter_get_by_name("buffersink"),
            "sink", nullptr, nullptr, graph_), "创建视频 buffersink");
        raw_inputs = avfilter_inout_alloc();
        if (!raw_inputs) throw std::bad_alloc();
        raw_inputs->name = av_strdup("out");
        if (!raw_inputs->name) throw std::bad_alloc();
        raw_inputs->filter_ctx = sink_;
        raw_inputs->pad_idx = 0;
        // 下面只拼接 FFmpeg 滤镜表达式；真正构建连接和检查格式由 parse_ptr/config 完成。
        std::ostringstream filter;
        if (layout == "grid") {
            for (int i = 0; i < 4; ++i)
                filter << "[in" << i << "]scale=" << width / 2 << ':' << height / 2
                       << ",setsar=1[v" << i << "];";
            filter << "[v0][v1][v2][v3]xstack=inputs=4:layout=0_0|w0_0|0_h0|w0_h0"
                      ",format=pix_fmts=yuv420p[out]";
        } else if (layout == "split") {
            for (int i = 0; i < 2; ++i)
                filter << "[in" << i << "]scale=" << width / 2 << ':' << height
                       << ",setsar=1[v" << i << "];";
            filter << "[v0][v1]hstack=inputs=2,format=pix_fmts=yuv420p[out]";
        } else {
            filter << "[in1]scale=" << (width / 3 / 2 * 2) << ':' << (height / 3 / 2 * 2)
                   << ",setsar=1[small];[in0][small]overlay=x=W-w:y=H-h:format=yuv420"
                      ",format=pix_fmts=yuv420p[out]";
        }
        ff_check(avfilter_graph_parse_ptr(graph_, filter.str().c_str(),
                                          &raw_inputs, &raw_outputs, nullptr), "连接视频滤镜");
        if (raw_inputs || raw_outputs) throw std::runtime_error("视频滤镜存在未连接端点");
        // 让 FFmpeg 协商各节点的格式、尺寸和时间基；成功后才允许向 buffer 送帧。
        ff_check(avfilter_graph_config(graph_, nullptr), "配置视频滤镜图");
    } catch (...) {
        avfilter_inout_free(&raw_inputs);
        avfilter_inout_free(&raw_outputs);
        throw;
    }
}
Frame VideoMixer::mix(const std::vector<const AVFrame*>& frames, int64_t tick) {
    if (frames.size() < sources_.size()) throw std::runtime_error("合成源数量不足");
    for (size_t i = 0; i < sources_.size(); ++i) {
        Frame reference = make_frame();
        ff_check(av_frame_ref(reference.get(), frames[i]), "引用待合成帧");
        reference->pts = tick;
        // flags=0且帧为引用计数帧时，成功送入会转移reference的缓冲引用。
        // 原始输入仍由上层持有；滤镜只接管这里新建的引用。
        ff_check(av_buffersrc_add_frame_flags(sources_[i], reference.get(), 0), "输入合成帧");
    }
    // sink 把节目图像引用放进新帧头；返回帧由调用者拥有，不依赖本次局部 reference 继续存在。
    Frame output = make_frame();
    ff_check(av_buffersink_get_frame(sink_, output.get()), "获取合成帧");
    if (output->pts != tick) throw std::runtime_error("合成输出时间戳不匹配");
    return output;
}
}
