/*
 * 完整节目调度中心。输入线程提供解码帧；本线程选帧/混音/合成/编码；输出与录制各有工作线程。
 * 阅读run_live中的编号段落：建源 → 建输出 → 分流回调 → 调度 → 视频/音频处理 → 排空退出。
 * 时间单位：tick=视频帧序号，cursor=48kHz采样位置，us=节目微秒，wall_start=单调时钟微秒。
 */
#include "manager/live_pipeline.h"
#include "input/video_source.h"
#include "input/audio_source.h"
#include "mixer/video_mixer.h"
#include "mixer/audio_mixer.h"
#include "output/async_output.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
extern "C" {
#include <libavutil/imgutils.h>
}
namespace esports {
namespace {
struct StatsFile {
    FILE* file = nullptr;
    explicit StatsFile(const std::string& path) {
        if (path.empty()) return;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0) ff_check(AVERROR(errno), "创建同步报告（不覆盖已有文件）");
        file = fdopen(fd, "w");
        if (!file) { ::close(fd); throw std::runtime_error("打开同步报告流失败"); }
        if (std::fprintf(file, "media_us,video_end_us,audio_end_us,av_delta_us,wall_lag_us,output_video,output_audio,output_kbps,dropped_video,dropped_audio,queue_packets,queue_bytes,max_queue_us,max_write_us,retries\n") < 0) {
            std::fclose(file); file = nullptr;
            throw std::runtime_error("同步报告写头失败");
        }
    }
    ~StatsFile() { if (file) std::fclose(file); }
    void write(int64_t t, int64_t v, int64_t a, int64_t lag, const OutputStats& output) {
        if (file && (std::fprintf(file, "%lld,%lld,%lld,%lld,%lld,%llu,%llu,%.3f,%llu,%llu,%zu,%zu,%lld,%lld,%llu\n",
            static_cast<long long>(t), static_cast<long long>(v), static_cast<long long>(a),
            static_cast<long long>(v-a), static_cast<long long>(lag),
            static_cast<unsigned long long>(output.video_packets),
            static_cast<unsigned long long>(output.audio_packets),
            output.elapsed_us > 0 ? output.encoded_bytes * 8000.0 / output.elapsed_us : 0,
            static_cast<unsigned long long>(output.queue.dropped_video),
            static_cast<unsigned long long>(output.queue.dropped_audio),
            output.queue.packets, output.queue.bytes,
            static_cast<long long>(output.max_queue_us), static_cast<long long>(output.max_write_us),
            static_cast<unsigned long long>(output.retries)) < 0 || std::fflush(file) != 0))
            throw std::runtime_error("同步报告写入失败");
    }
};
}
int run_live(const LiveOptions& options, const volatile std::sig_atomic_t& stop, Logger& log) {
    NetworkRuntime network;
    const bool network_output = starts_with(options.output, "rtmp://");
    AsyncOutput output(options.output_options, network_output, log);
    StatsFile stats(options.stats_file);
    MetricsFile metrics(options.metrics_file);
    uint64_t processing_us = 0;
    Recorder recorder(options.recording, log);
    MediaEncoder video_encoder, audio_encoder;
    VideoMixer video_mixer;
    AudioMixer audio_mixer(options.audio_inputs.size());
    // ① 输入线程统一托管：先通知所有源停止，再逐一join，防止某个源卡在满队列。
    struct Sources {
        std::vector<std::unique_ptr<VideoSource>> videos;
        std::vector<std::unique_ptr<AudioSource>> audios;
        void shutdown() {
            for (auto& source : videos) source->stop();
            for (auto& source : audios) source->stop();
            for (auto& source : videos) source->join();
            for (auto& source : audios) source->join();
        }
        ~Sources() { shutdown(); }
    } group;
    int64_t video_count = 0, audio_samples = 0, silence_samples = 0, peak_delta = 0, skipped_video = 0, skipped_audio = 0;
    const auto report_metrics = [&] {
        if (!metrics.enabled()) return;
        PipelineMetrics data;
        for (size_t i = 0; i < group.videos.size(); ++i) {
            data.video_frames[i] = group.videos[i]->metrics().units.load();
            data.decode_us += group.videos[i]->metrics().total_us.load();
            data.video_queued += group.videos[i]->queue().size();
        }
        for (size_t i = 0; i < group.audios.size(); ++i) {
            data.audio_frames[i] = group.audios[i]->metrics().units.load();
            data.decode_us += group.audios[i]->metrics().total_us.load();
            data.audio_queued += group.audios[i]->queue().size();
            data.audio_late += group.audios[i]->queue().late();
        }
        data.encode_us = video_encoder.metrics().total_us.load() + audio_encoder.metrics().total_us.load();
        data.processing_us = processing_us;
        data.skipped_video = skipped_video; data.skipped_audio = skipped_audio;
        const auto sent = output.snapshot();
        data.output_video = sent.video_packets;
        data.output_dropped = sent.queue.dropped_video + sent.queue.dropped_audio;
        metrics.sample(data);
    };
    try {
        group.videos.reserve(options.inputs.size());
        group.audios.reserve(options.audio_inputs.size());
        for (size_t i = 0; i < options.inputs.size(); ++i)
            group.videos.emplace_back(new VideoSource(options.inputs[i], options.width,
                options.height, options.timeout_ms, log, options.video_offsets[i], options.video_queue_frames));
        for (size_t i = 0; i < options.audio_inputs.size(); ++i) {
            group.audios.emplace_back(new AudioSource(options.audio_inputs[i], options.timeout_ms,
                                                      options.audio_offsets[i], log, options.audio_queue_blocks));
            audio_mixer.set_gain(i, options.gains[i]);
            audio_mixer.set_muted(i, options.muted[i]);
        }
        // 等待首批帧或失败状态，但设置总期限；单路失效不会无限阻塞整个节目启动。
        const int64_t startup = av_gettime_relative() + int64_t(options.timeout_ms + 1000) * 1000;
        while (!stop) {
            bool ready = true;
            for (auto& source : group.videos) ready = ready && source->queue().ready();
            for (auto& source : group.audios) ready = ready && source->queue().ready();
            if (ready || av_gettime_relative() >= startup) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (stop) return 130;
        bool usable = false;
        for (auto& source : group.videos) usable = usable || (source->queue().ready() && !source->queue().exhausted());
        for (auto& source : group.audios) usable = usable || (source->queue().ready() && !source->queue().exhausted());
        if (!usable) throw std::runtime_error("所有音视频源均失败或没有媒体");

        // ② 建立降级黑帧、编码器、异步输出、录制和合成器；源失效时仍能形成节目。
        Frame black = make_frame();
        black->format = AV_PIX_FMT_YUV420P;
        black->width = options.width; black->height = options.height;
        ff_check(av_frame_get_buffer(black.get(), 32), "创建同步黑帧");
        ptrdiff_t lines[4] = {black->linesize[0], black->linesize[1], black->linesize[2], 0};
        ff_check(av_image_fill_black(black->data, lines, AV_PIX_FMT_YUV420P,
                                    AVCOL_RANGE_MPEG, options.width, options.height), "填充同步黑帧");
        VideoEncodingOptions encoding;
        encoding.low_latency = true;
        encoding.bitrate = options.video_bitrate;
        encoding.gop_frames = std::max(1, options.fps * options.gop_ms / 1000);
        encoding.vbv_ms = options.vbv_ms;
        video_encoder.open_video(options.width, options.height, options.fps, encoding);
        audio_encoder.open_audio();
        output.open(options.output, {video_encoder.context(), audio_encoder.context()});
        recorder.open({video_encoder.context(), audio_encoder.context()});
        video_mixer.configure(options.layout, options.width, options.height, options.fps);
        // ③ 编码输出在当前线程同步回调：先为录制克隆引用，再把原包移动给推流队列。
        // 流编号固定为视频0/音频1；传递的是编码器time_base，封装器稍后再次换算。
        const PacketConsumer video_packet = [&](Packet p) {
            p->stream_index = 0; recorder.submit(p.get(), video_encoder.context()->time_base); output.submit(std::move(p), video_encoder.context()->time_base);
        };
        const PacketConsumer audio_packet = [&](Packet p) {
            p->stream_index = 1; recorder.submit(p.get(), audio_encoder.context()->time_base); output.submit(std::move(p), audio_encoder.context()->time_base);
        };
        std::vector<Frame> current(group.videos.size());
        // ④ 节目时钟不直接用输入PTS驱动：视频tick/fps秒，音频cursor/48000秒。
        // 各源已经归一化，调度器按节目时刻选择对应画面和音频窗口。
        int64_t tick = 0, cursor = 0, last_end = 0, next_report = 0;
        int64_t video_end = 0, audio_end = 0;
        const int64_t wall_start = av_gettime_relative();
        bool switched = false, gain_changed = false;
        while (!stop) {
            output.check();
            if (network_output && options.realtime) {
                const int64_t elapsed = av_gettime_relative() - wall_start;
                const int64_t scheduled = std::min(
                    av_rescale_q(tick, AVRational{1, options.fps}, AVRational{1, AV_TIME_BASE}),
                    av_rescale_q(cursor, AVRational{1, 48000}, AVRational{1, AV_TIME_BASE}));
                // 实时网络落后过多时跳到当前节目时刻，不追发全部历史积压；音频对齐编码块边界。
                // 同时使旧编码窗口失效并请求新IDR，避免跳过帧后继续发送断裂参考链。
                if (elapsed - scheduled > int64_t(options.output_options.queue.latency_ms) * 1000) {
                    const int64_t new_tick = av_rescale_rnd(elapsed, options.fps, AV_TIME_BASE, AV_ROUND_UP);
                    const int64_t now_samples = av_rescale_rnd(elapsed, 48000, AV_TIME_BASE, AV_ROUND_UP);
                    const int block = audio_encoder.context()->frame_size;
                    const int64_t new_cursor = (now_samples + block - 1) / block * block;
                    skipped_video += std::max<int64_t>(0, new_tick - tick);
                    skipped_audio += std::max<int64_t>(0, new_cursor - cursor);
                    tick = std::max(tick, new_tick); cursor = std::max(cursor, new_cursor);
                    output.reset_for_latency();
                }
            }
            bool exhausted = true;
            for (size_t i = 0; i < group.videos.size(); ++i) {
                exhausted = exhausted && group.videos[i]->queue().exhausted();
                if (current[i]) last_end = std::max(last_end, current[i]->pts + current[i]->duration);
            }
            for (auto& source : group.audios) {
                exhausted = exhausted && source->queue().exhausted();
                last_end = std::max(last_end, av_rescale_q(source->queue().last_end(),
                                    AVRational{1, 48000}, AVRational{1, AV_TIME_BASE}));
            }
            // 比较两条轨道下一次处理时刻，先处理较早的一条；不能直接比较tick和cursor整数。
            const bool video_turn = av_compare_ts(tick, AVRational{1, options.fps},
                                                   cursor, AVRational{1, 48000}) <= 0;
            const int64_t us = video_turn
                ? av_rescale_q(tick, AVRational{1, options.fps}, AVRational{1, AV_TIME_BASE})
                : av_rescale_q(cursor, AVRational{1, 48000}, AVRational{1, AV_TIME_BASE});
            const int64_t end = options.duration_ms ? int64_t(options.duration_ms) * 1000
                                                   : (exhausted ? last_end : INT64_MAX);
            if (us >= end) break;
            if (options.realtime) {
                while (!stop) {
                    const int64_t delay = us - (av_gettime_relative() - wall_start);
                    if (delay <= 0) break;
                    std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(delay, 10000)));
                }
            }
            if (stop) break;
            // ⑤ 视频：取每路不晚于节目时刻的最新帧；短缺沿用上一帧，过期则黑帧。
            if (video_turn) {
                std::vector<const AVFrame*> frames;
                for (size_t i = 0; i < current.size(); ++i) {
                    FrameQueue& q = group.videos[i]->queue();
                    if (options.realtime) q.advance(us, current[i]);
                    else {
                        const int64_t deadline = av_gettime_relative() + int64_t(options.timeout_ms) * 1000;
                        while (!stop && !q.advance(us, current[i], 10) && av_gettime_relative() < deadline) {}
                    }
                    frames.push_back(current[i] && us - current[i]->pts <= int64_t(options.stale_ms) * 1000
                                     ? current[i].get() : black.get());
                }
                if (stop) break;
                if (!switched && options.switch_at_ms >= 0 && us >= int64_t(options.switch_at_ms) * 1000) {
                    video_mixer.configure(options.switch_layout, options.width, options.height, options.fps);
                    switched = true;
                }
                const int64_t mix_start = av_gettime_relative();
                // 合成滤镜按1/fps计时；输出给编码器之前换到其1/90000时间基。
                Frame frame = video_mixer.mix(frames, tick);
                processing_us += av_gettime_relative() - mix_start;
                frame->pts = av_rescale_q(tick, AVRational{1, options.fps}, video_encoder.context()->time_base);
                if (output.take_keyframe_request()) frame->pict_type = AV_PICTURE_TYPE_I;
                video_encoder.send(frame.get(), video_packet);
                ++tick; ++video_count;
                video_end = av_rescale_q(tick, AVRational{1, options.fps}, AVRational{1, AV_TIME_BASE});
            } else {
                // ⑥ 音频：按AAC输入块长创建同一采样窗口；最后一个窗口可能不足整块。
                int count = audio_encoder.context()->frame_size;
                if (end != INT64_MAX)
                    count = static_cast<int>(std::min<int64_t>(count,
                        av_rescale_q(end, AVRational{1, AV_TIME_BASE}, AVRational{1, 48000}) - cursor));
                if (count <= 0) break;
                std::vector<Frame> blocks;
                std::vector<const AVFrame*> frames;
                for (auto& source : group.audios) {
                    // 先预填静音再覆盖队列中已有数据。实时模式不等慢源，离线模式可有限等待。
                    Frame block = audio_frame(count, cursor);
                    int filled = 0;
                    if (options.realtime) source->queue().read_window(block.get(), filled, 0);
                    else {
                        const int64_t deadline = av_gettime_relative() + int64_t(options.timeout_ms) * 1000;
                        while (!stop && !source->queue().read_window(block.get(), filled, 10) &&
                               av_gettime_relative() < deadline) {}
                    }
                    silence_samples += count - filled;
                    frames.push_back(block.get());
                    blocks.push_back(std::move(block));
                }
                if (stop) break;
                if (!gain_changed && options.change_at_ms >= 0 && us >= int64_t(options.change_at_ms) * 1000) {
                    audio_mixer.set_gain(options.change_index, options.change_gain);
                    gain_changed = true;
                }
                const int64_t mix_start = av_gettime_relative();
                Frame mixed = audio_mixer.mix(frames, cursor, count);
                processing_us += av_gettime_relative() - mix_start;
                audio_encoder.send(mixed.get(), audio_packet);
                // 音频位置按实际采样数推进，避免用毫秒取整造成长时间累积误差。
                cursor += count; audio_samples += count;
                audio_end = av_rescale_q(cursor, AVRational{1, 48000}, AVRational{1, AV_TIME_BASE});
            }
            if (video_end && audio_end)
                peak_delta = std::max(peak_delta, static_cast<int64_t>(std::llabs(video_end - audio_end)));
            if (us >= next_report) {
                stats.write(us, video_end, audio_end, (av_gettime_relative() - wall_start) - us, output.snapshot());
                report_metrics();
                next_report = us + AV_TIME_BASE;
            }
        }
        // ⑦ 正常结束排空编码器尾包，再排空输出/录制队列；用户停止则优先中断推流等待。
        if (!stop) {
            video_encoder.send(nullptr, video_packet);
            audio_encoder.send(nullptr, audio_packet);
        }
        if (stop) output.cancel();
        output.finish(&stop);
        recorder.finish();
        stats.write(std::max(video_end, audio_end), video_end, audio_end,
                    (av_gettime_relative() - wall_start) - std::max(video_end, audio_end), output.snapshot());
    } catch (const std::exception&) {
        output.cancel();
        try { output.finish(&stop); } catch (const std::exception& e) { log.log(LogLevel::Warning, e.what()); }
        if (!stop) throw;
        return 130;
    }
    // 停止并回收所有输入线程后采最终统计；异常路径由Sources析构执行同样的回收。
    group.shutdown();
    report_metrics();
    for (size_t i = 0; i < group.audios.size(); ++i)
        log.log(LogLevel::Info, "音频队列 " + std::to_string(i) + " 峰值=" +
            std::to_string(group.audios[i]->queue().peak()) + "/" + std::to_string(options.audio_queue_blocks) + "，过期块=" +
            std::to_string(group.audios[i]->queue().late()));
    log.log(LogLevel::Info, "同步输出：视频帧=" + std::to_string(video_count) +
        "，音频采样=" + std::to_string(audio_samples) + "，调度音画差峰值us=" +
        std::to_string(peak_delta) + "，各源补静音采样总数=" + std::to_string(silence_samples) +
        "，限幅采样=" + std::to_string(audio_mixer.limited_samples()));
    const OutputStats sent = output.snapshot();
    log.log(LogLevel::Info, "输出统计：视频包=" + std::to_string(sent.video_packets) +
        "，音频包=" + std::to_string(sent.audio_packets) +
        "，平均fps=" + std::to_string(sent.elapsed_us > 0 ? sent.video_packets * 1000000.0 / sent.elapsed_us : 0) +
        "，编码数据kbps=" + std::to_string(sent.elapsed_us > 0 ? sent.encoded_bytes * 8000.0 / sent.elapsed_us : 0) +
        "，丢视频包=" + std::to_string(sent.queue.dropped_video) +
        "，丢音频包=" + std::to_string(sent.queue.dropped_audio) +
        "，视频丢包率%=" + std::to_string(sent.video_packets + sent.queue.dropped_video ?
            100.0 * sent.queue.dropped_video / (sent.video_packets + sent.queue.dropped_video) : 0) +
        "，音频丢包率%=" + std::to_string(sent.audio_packets + sent.queue.dropped_audio ?
            100.0 * sent.queue.dropped_audio / (sent.audio_packets + sent.queue.dropped_audio) : 0) +
        "，重试=" + std::to_string(sent.retries) +
        "，队列峰值包/字节=" + std::to_string(sent.queue.peak_packets) + "/" + std::to_string(sent.queue.peak_bytes) +
        "，排队/写入峰值us=" + std::to_string(sent.max_queue_us) + "/" + std::to_string(sent.max_write_us) +
        "，跳过调度视频帧=" + std::to_string(skipped_video) + "，音频采样=" + std::to_string(skipped_audio));
    return stop ? 130 : 0;
}
}
