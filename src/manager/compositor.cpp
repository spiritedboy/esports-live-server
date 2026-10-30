#include "manager/compositor.h"
#include "input/video_source.h"
#include "mixer/video_mixer.h"
#include "output/stream_output.h"
#include <algorithm>
#include <chrono>
extern "C" {
#include <libavutil/imgutils.h>
}
namespace esports {
int run_composite(const CompositeOptions& options, const volatile std::sig_atomic_t& stop, Logger& log) {
    NetworkRuntime network;
    IoControl io(stop, options.timeout_ms);
    StreamOutput output(io);
    MediaEncoder encoder;
    VideoMixer mixer;
    struct Sources {
        std::vector<std::unique_ptr<VideoSource>> sources;
        ~Sources() {
            for (auto& source : sources) source->stop();
            for (auto& source : sources) source->join();
        }
    } group;
    int64_t produced = 0;
    try {
        group.sources.reserve(options.inputs.size());
        for (const std::string& url : options.inputs)
            group.sources.emplace_back(new VideoSource(url, options.width, options.height,
                                                       options.timeout_ms, log));
        const int64_t deadline = av_gettime_relative() + int64_t(options.timeout_ms + 1000) * 1000;
        while (!stop) {
            bool ready = true;
            for (auto& source : group.sources) ready = ready && source->queue().ready();
            if (ready || av_gettime_relative() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (stop) return 130;
        std::vector<Frame> current(group.sources.size());
        bool usable = false;
        for (size_t i = 0; i < current.size(); ++i) {
            group.sources[i]->queue().advance(0, current[i]);
            usable = usable || static_cast<bool>(current[i]);
        }
        if (!usable) throw std::runtime_error("所有合成源均未提供首帧");

        Frame black = make_frame();
        black->format = AV_PIX_FMT_YUV420P;
        black->width = options.width;
        black->height = options.height;
        ff_check(av_frame_get_buffer(black.get(), 32), "创建黑帧");
        ptrdiff_t lines[4] = {black->linesize[0], black->linesize[1], black->linesize[2], 0};
        ff_check(av_image_fill_black(black->data, lines, AV_PIX_FMT_YUV420P,
                                    AVCOL_RANGE_MPEG, options.width, options.height), "填充黑帧");
        encoder.open_video(options.width, options.height, options.fps);
        output.open_encoded(options.output, {encoder.context()}, log);
        mixer.configure(options.layout, options.width, options.height, options.fps);
        const PacketConsumer consume = [&](Packet packet) {
            packet->stream_index = 0;
            output.write(packet.get(), encoder.context()->time_base, 0);
        };
        const int64_t wall_start = av_gettime_relative();
        bool switched = false;
        for (int64_t tick = 0; !stop; ++tick) {
            const int64_t us = av_rescale_q(tick, AVRational{1, options.fps}, AVRational{1, AV_TIME_BASE});
            if (options.duration_ms && us >= int64_t(options.duration_ms) * 1000) break;
            if (options.realtime) {
                while (!stop) {
                    const int64_t delay = us - (av_gettime_relative() - wall_start);
                    if (delay <= 0) break;
                    std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(delay, 10000)));
                }
            }
            if (stop) break;
            bool exhausted = true;
            int64_t last_end = 0;
            std::vector<const AVFrame*> selected;
            for (size_t i = 0; i < current.size(); ++i) {
                FrameQueue& queue = group.sources[i]->queue();
                if (options.realtime) queue.advance(us, current[i]);
                else {
                    const int64_t wait_deadline = av_gettime_relative() + int64_t(options.timeout_ms) * 1000;
                    while (!stop && !queue.advance(us, current[i], 10)) {
                        if (av_gettime_relative() >= wait_deadline) break;
                    }
                }
                exhausted = exhausted && queue.exhausted();
                if (current[i]) last_end = std::max(last_end, current[i]->pts + current[i]->duration);
                const bool fresh = current[i] && us - current[i]->pts <= int64_t(options.stale_ms) * 1000;
                selected.push_back(fresh ? current[i].get() : black.get());
            }
            if (stop) break;
            if (!options.duration_ms && exhausted && us >= last_end) break;
            if (!switched && options.switch_at_ms >= 0 && us >= int64_t(options.switch_at_ms) * 1000) {
                mixer.configure(options.switch_layout, options.width, options.height, options.fps);
                switched = true;
                log.log(LogLevel::Info, "布局已切换为 " + options.switch_layout);
            }
            Frame frame = mixer.mix(selected, tick);
            frame->pts = av_rescale_q(tick, AVRational{1, options.fps}, encoder.context()->time_base);
            encoder.send(frame.get(), consume);
            ++produced;
        }
        if (!stop) encoder.send(nullptr, consume);
        output.finish();
    } catch (const std::exception&) {
        try { output.finish(); }
        catch (const std::exception& e) { log.log(LogLevel::Warning, e.what()); }
        if (!stop) throw;
        return 130;
    }
    for (size_t i = 0; i < group.sources.size(); ++i)
        log.log(LogLevel::Info, "源 " + std::to_string(i) + " 队列峰值=" +
            std::to_string(group.sources[i]->queue().peak()) + "/8" +
            (group.sources[i]->failed() ? "（源失败，已降级）" : ""));
    log.log(LogLevel::Info, "合成视频帧数=" + std::to_string(produced));
    return stop ? 130 : 0;
}
}
