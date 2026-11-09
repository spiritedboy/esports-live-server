/*
 * 初学者导读
 * 这是独立录像工具：选择片段 → 找关键帧 → 复制压缩包并重封装，全程不解码/重编码。
 * 因此不能精确裁到任意视频帧；请求开始时间会向前对齐关键帧，实际片段可能更长。
 * 索引记录节目微秒位置，定位后换成分段内部毫秒；当前工具不支持一次请求跨多个分段。
 * extract 尽快写新文件，replay 按 DTS 相对墙钟实时发送，也可以输出 RTMP。
 * 本工具仅接受本项目无 B 帧 H.264 录像；不能将它理解成通用的任意格式剪辑器。
 * 它不会把录像插入正在运行的 live 节目；回放和直播是不同进程入口。
 * 正常 EOF 后显式 mux.finish，保证容器尾完成；Ctrl+C 的退出码为 130。
 */
/*
 * 录像提取/回放入口：索引定位 → 向前找关键帧 → 重设时间原点 → 转封装。
 * 不重新编码，不做精确逐帧裁切，也不会将录像插入正在运行的直播节目。
 */
#include "input/stream_input.h"
#include "output/stream_output.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <limits>
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop_handler(int) { stopped = 1; }
// 只接受非负十进制整数毫秒，并限制到一天；stoll 溢出会由 main 的 catch 处理。
int64_t milliseconds(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("时间必须为非负整数毫秒");
    const auto value = std::stoll(s);
    if (value > 86400000) throw std::runtime_error("时间超过24小时");
    return value;
}
bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size()-tail.size(), tail.size(), tail) == 0;
}
}
int main(int argc, char** argv) {
    std::signal(SIGINT, stop_handler); std::signal(SIGTERM, stop_handler); std::signal(SIGPIPE, SIG_IGN);
    esports::Logger log(std::cerr);
    try {
        std::string input, output, index_path, mode = "extract";
        int64_t start_ms = 0, duration_ms = 0;
        bool list = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key(argv[i]);
            if (key == "--help") {
                std::cout << "esports_replay --input 录像.mp4|mkv（或 --index 录像.index.tsv） [--list-keyframes] "
                    "[--output 新文件.mp4|mkv|flv|rtmp://... --mode extract|replay --start-ms 0 --duration-ms 0]\n"
                    "extract快速提取；replay按DTS实时发送。起点向前对齐关键帧，0时长表示到EOF。\n";
                return 0;
            }
            if (key == "--list-keyframes") { list = true; continue; }
            if (++i == argc) throw std::runtime_error("缺少参数值");
            const std::string value(argv[i]);
            if (key == "--input") input = value;
            else if (key == "--index") index_path = value;
            else if (key == "--output") output = value;
            else if (key == "--mode") mode = value;
            else if (key == "--start-ms") start_ms = milliseconds(value);
            else if (key == "--duration-ms") duration_ms = milliseconds(value);
            else throw std::runtime_error("未知参数：" + key);
        }
        // 索引记录绝对节目时间，选中 [start_us,end_us) 后换成分段内时间；不自动拼接下一段。
        if (!index_path.empty()) {
            if (!input.empty() || list) throw std::runtime_error("索引定位不可同时指定input或list-keyframes");
            std::ifstream index(index_path);
            std::string line;
            if (!std::getline(index, line) || line != "segment\tstart_us\tend_us\tpath")
                throw std::runtime_error("录像索引头无效");
            bool found = false;
            while (std::getline(index, line)) {
                std::istringstream row(line);
                std::string number, begin, end, path;
                if (!std::getline(row, number, '\t') || !std::getline(row, begin, '\t') ||
                    !std::getline(row, end, '\t') || !std::getline(row, path) || path.empty())
                    throw std::runtime_error("录像索引行损坏");
                size_t used = 0;
                const int64_t first = std::stoll(begin, &used);
                if (used != begin.size() || first < 0) throw std::runtime_error("索引起点无效");
                const int64_t last = std::stoll(end, &used);
                if (used != end.size() || last <= first) throw std::runtime_error("索引终点无效");
                if (start_ms * 1000 >= first && start_ms * 1000 < last) {
                    if (duration_ms && (start_ms + duration_ms) * 1000 > last)
                        throw std::runtime_error("基础提取范围不能跨分段，请分别提取各段");
                    input = path;
                    start_ms -= first / 1000;
                    found = true;
                    log.log(esports::LogLevel::Info, "索引定位分段：" + path);
                    break;
                }
            }
            if (!found) throw std::runtime_error("索引中无对应时间的已完成分段");
        }
        if (input.empty() || input.find(':') != std::string::npos || (mode != "extract" && mode != "replay"))
            throw std::runtime_error("需要本地录像和有效模式");
        if (!list && output.empty()) throw std::runtime_error("缺少输出路径");
        const bool network = esports::starts_with(output, "rtmp://");
        if (network && mode != "replay") throw std::runtime_error("RTMP输出必须使用replay实时模式");
        if (!network && output.find(':') != std::string::npos) throw std::runtime_error("不支持的输出协议");
        std::string format = network || ends_with(output, ".flv") ? "flv" :
            ends_with(output, ".mp4") ? "mp4" : ends_with(output, ".mkv") ? "matroska" : "";
        if (!list && format.empty()) throw std::runtime_error("输出扩展名必须为mp4/mkv/flv");
        esports::NetworkRuntime runtime;
        esports::IoControl io(stopped, 3000);
        esports::StreamInput source(io); source.open(input, log);
        AVFormatContext* context = source.context();
        const int video = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        esports::ff_check(video, "定位录像视频轨");
        if (context->streams[video]->codecpar->codec_id != AV_CODEC_ID_H264 ||
            context->streams[video]->codecpar->video_delay != 0)
            throw std::runtime_error("基础提取仅支持本项目无B帧H.264录像");
        const int64_t base = context->start_time == AV_NOPTS_VALUE ? 0 : context->start_time;
        if (context->duration != AV_NOPTS_VALUE && start_ms * 1000 >= context->duration)
            throw std::runtime_error("请求起点超出录像时长");
        esports::Packet packet = esports::make_packet();
        // 列出关键帧 DTS 相对容器起点的微秒位置及原文件字节位置，只读扫描，不创建输出。
        if (list) {
            std::cout << "keyframe_us\tbyte_position\n";
            while (!stopped) {
                const int result = source.read(packet.get());
                if (result == AVERROR_EOF) break;
                if (result == AVERROR(EAGAIN)) continue;
                if (packet->stream_index == video && (packet->flags & AV_PKT_FLAG_KEY))
                    std::cout << av_rescale_q(packet->dts, context->streams[video]->time_base,
                        AVRational{1, AV_TIME_BASE}) - base << '\t' << packet->pos << '\n';
                av_packet_unref(packet.get());
            }
            return stopped ? 130 : 0;
        }
        io.arm();
        // seek 指定 video 索引时 target 必须采用该视频轨道 time_base，而不是默认微秒。
        const int64_t target = av_rescale_q(base + start_ms * 1000, AVRational{1, AV_TIME_BASE},
                                           context->streams[video]->time_base);
        // target用选定视频流time_base，向请求时间之前的关键帧定位；实际片段可能比请求更早。
        esports::ff_check(av_seek_frame(context, video, target, AVSEEK_FLAG_BACKWARD), "定位前方关键帧");
        avformat_flush(context);
        esports::StreamOutput mux(io, network, format);
        bool opened = false;
        int64_t origin = 0, wall = 0;
        const int64_t limit = duration_ms ? base + (start_ms + duration_ms) * 1000 : std::numeric_limits<int64_t>::max();
        std::vector<bool> ended(context->nb_streams, false);
        while (!stopped) {
            const int result = source.read(packet.get());
            if (result == AVERROR_EOF) break;
            if (result == AVERROR(EAGAIN)) continue;
            const int index = packet->stream_index;
            const AVRational tb = context->streams[index]->time_base;
            if (packet->dts == AV_NOPTS_VALUE || packet->pts == AV_NOPTS_VALUE)
                throw std::runtime_error("录像包缺少时间戳");
            const int64_t dts = av_rescale_q(packet->dts, tb, AVRational{1, AV_TIME_BASE});
            // seek 后读到的第一包未必是所需关键帧，继续跳过直到可独立解码的视频起点才写输出头。
            if (!opened) {
                if (index != video || !(packet->flags & AV_PKT_FLAG_KEY)) {
                    av_packet_unref(packet.get()); continue;
                }
                if (dts >= limit) throw std::runtime_error("请求范围内无可用关键帧");
                origin = dts; wall = av_gettime_relative();
                mux.open(output, context, log); opened = true;
                log.log(esports::LogLevel::Info, "实际片段起点us=" + std::to_string(origin-base) +
                    "，请求起点us=" + std::to_string(start_ms*1000));
            }
            if (!mux.selected(index) || dts < origin) { av_packet_unref(packet.get()); continue; }
            // 每条已选择轨道都到达结束边界才停止，避免交错读取时先截掉另一轨的尾包。
            if (dts >= limit) {
                ended[index] = true;
                bool all = true;
                for (unsigned i = 0; i < context->nb_streams; ++i)
                    if (mux.selected(i) && !ended[i]) all = false;
                av_packet_unref(packet.get());
                if (all) break;
                continue;
            }
            // extract不等待；replay按DTS相对起点匹配单调墙钟，避免把整段录像瞬间灌入网络。
            if (mode == "replay") {
                while (!stopped && av_gettime_relative() - wall < dts - origin)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (stopped) break;
            }
            mux.write(packet.get(), tb, origin);
            av_packet_unref(packet.get());
        }
        if (!opened && !stopped) throw std::runtime_error("录像中无可回放数据");
        mux.finish();
        return stopped ? 130 : 0;
    } catch (const std::exception& e) {
        log.log(esports::LogLevel::Error, e.what());
        return stopped ? 130 : 1;
    }
}
