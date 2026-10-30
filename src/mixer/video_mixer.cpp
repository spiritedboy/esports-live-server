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
        ff_check(av_buffersrc_add_frame_flags(sources_[i], reference.get(), 0), "输入合成帧");
    }
    Frame output = make_frame();
    ff_check(av_buffersink_get_frame(sink_, output.get()), "获取合成帧");
    if (output->pts != tick) throw std::runtime_error("合成输出时间戳不匹配");
    return output;
}
}
