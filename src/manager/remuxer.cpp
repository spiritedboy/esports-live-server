/*
 * 最短媒体链路：StreamInput → 压缩AVPacket → StreamOutput，无解码重编码。
 * 整条链路在调用线程执行，以DTS控制可选的实时发送节奏。
 */
#include "manager/remuxer.h"
#include "input/stream_input.h"
#include "output/stream_output.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace esports {
int run_remux(const RemuxOptions& options, const volatile std::sig_atomic_t& stop, Logger& log) {
    NetworkRuntime network;
    IoControl io(stop, options.timeout_ms);
    StreamInput input(io);
    StreamOutput output(io);
    Packet packet = make_packet();
    int64_t count = 0;
    bool eof = false;
    try {
        input.open(options.input, log);
        output.open(options.output, input.context(), log);
        int64_t origin_us = AV_NOPTS_VALUE;
        int64_t start_us = 0;
        int64_t last_progress_us = av_gettime_relative();
        while (!io.stopped()) {
            const int result = input.read(packet.get());
            if (result == AVERROR_EOF) {
                if (is_network_input(options.input))
                    throw std::runtime_error("网络输入 EOF：连接结束或断流，Phase 1 不自动重连");
                eof = true;
                break;
            }
            if (result == AVERROR(EAGAIN)) {
                if (av_gettime_relative() - last_progress_us > options.timeout_ms * int64_t(1000))
                    throw std::runtime_error("输入持续 EAGAIN 超时");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            last_progress_us = av_gettime_relative();
            if (!output.selected(packet->stream_index)) continue;
            if ((packet->flags & AV_PKT_FLAG_CORRUPT) != 0)
                throw std::runtime_error("输入包损坏");
            if (packet->dts == AV_NOPTS_VALUE || packet->pts == AV_NOPTS_VALUE)
                throw std::runtime_error("缺少 PTS/DTS，Phase 1 不推测缺失时间戳");
            const AVRational tb = input.context()->streams[packet->stream_index]->time_base;
            const int64_t dts_us = av_rescale_q(packet->dts, tb, AVRational{1, AV_TIME_BASE});
            // 为后续微秒差值和等待计算保留安全余量。
            const int64_t timestamp_limit = int64_t(1) << 60;
            if (dts_us <= -timestamp_limit || dts_us >= timestamp_limit)
                throw std::runtime_error("输入 DTS 超出可处理范围");
            // 只选一个共同原点，音视频各减自己的首PTS会抹掉原有轨道偏移。
            if (origin_us == AV_NOPTS_VALUE) {
                origin_us = dts_us;
                start_us = av_gettime_relative();
            }
            // 文件读取远快于播放速度，按DTS相对原点限速；10ms分段等待便于及时响应停止。
            if (options.realtime) {
                const int64_t media_elapsed = dts_us - origin_us;
                while (!io.stopped()) {
                    const int64_t remaining = media_elapsed - (av_gettime_relative() - start_us);
                    if (remaining <= 0) break;
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(std::min<int64_t>(remaining, 10000)));
                }
            }
            if (io.stopped()) break;
            output.write(packet.get(), tb, origin_us);
            ++count;
        }
        output.finish();
    } catch (const std::exception&) {
        // 保留原始错误，尽力完成本地部分文件；清理失败单独记录。
        try { output.finish(); }
        catch (const std::exception& error) { log.log(LogLevel::Warning, error.what()); }
        if (!io.stopped()) throw;
        log.log(LogLevel::Warning, "用户停止，资源已回收");
        return 130;
    }
    log.log(LogLevel::Info, (eof ? "输入 EOF，" : "用户停止，") +
            std::string("已写入包数=") + std::to_string(count));
    if (io.stopped()) return 130;
    if (count == 0) throw std::runtime_error("输入结束但未写入任何媒体包");
    return 0;
}
} // namespace esports
