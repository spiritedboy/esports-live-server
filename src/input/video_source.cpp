/*
 * 一路视频一个工作线程：打开源 → 解码 → 缩放/像素转换 → 时间轴归一化 → FrameQueue。
 * 线程只生产帧，主处理线程负责选帧；输入失败后关闭队列，不在这里自动重连。
 */
#include "input/video_source.h"
#include "input/stream_input.h"
#include "codec/video_converter.h"
#include "sync/av_synchronizer.h"
#include <chrono>
#include <cerrno>
#include <pthread.h>
#include <sys/stat.h>
namespace esports {
VideoSource::VideoSource(const std::string& url, int width, int height, int timeout_ms, Logger& log, int offset_ms, size_t queue_frames)
    : queue_(queue_frames) {
    // 子线程继承阻塞掩码，确保 SIGINT/SIGTERM 只在主线程处理。
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    const int result = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (result != 0) ff_check(AVERROR(result), "屏蔽工作线程信号");
    try {
        worker_ = std::thread(&VideoSource::run, this, url, width, height, timeout_ms, std::ref(log), offset_ms);
    } catch (...) {
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        throw;
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}
VideoSource::~VideoSource() { stop(); join(); }
void VideoSource::stop() { stop_.store(true); queue_.close(); }
void VideoSource::join() { if (worker_.joinable()) worker_.join(); }
void VideoSource::run(std::string url, int width, int height, int timeout_ms, Logger& log, int offset_ms) {
    try {
        IoControl io(stop_, timeout_ms);
        StreamInput input(io);
        MediaDecoder decoder(&metrics_);
        VideoConverter converter;
        if (!is_network_input(url)) {
            struct stat info = {};
            if (stat(url.c_str(), &info) < 0) ff_check(AVERROR(errno), "检查视频源文件");
            if (!S_ISREG(info.st_mode)) throw std::runtime_error("视频源必须是普通文件");
        }
        input.open(url, log);
        AVFormatContext* format = input.context();
        int index = -1;
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (index >= 0) throw std::runtime_error("合成源仅允许一路视频");
                index = i;
            }
        }
        if (index < 0) throw std::runtime_error("合成源没有视频");
        AVStream* stream = format->streams[index];
        decoder.open(stream);
        const AVRational rate = av_guess_frame_rate(format, stream, nullptr);
        if (rate.num <= 0 || rate.den <= 0) throw std::runtime_error("无法确定源帧率");
        const int64_t default_duration = av_rescale_q(1, av_inv_q(rate), AVRational{1, AV_TIME_BASE});
        // 使用容器共同原点保留轨道偏移，容器无起点时才回退到首帧。
        int64_t origin = container_origin_us(format);
        int64_t previous = AV_NOPTS_VALUE;
        SourceClock clock(AV_TIME_BASE);
        const FrameConsumer consume = [&](Frame frame) {
            if (stop_) return;
            int64_t pts = AV_NOPTS_VALUE;
            if (frame->pts != AV_NOPTS_VALUE) {
                const int64_t timestamp = av_rescale_q(frame->pts, stream->time_base, AVRational{1, AV_TIME_BASE});
                if (timestamp < -(int64_t(1) << 50) || timestamp > (int64_t(1) << 50))
                    throw std::runtime_error("合成源时间戳超出范围");
                if (origin == AV_NOPTS_VALUE) origin = timestamp;
                if (origin < -(int64_t(1) << 50) || origin > (int64_t(1) << 50))
                    throw std::runtime_error("合成源起点超出范围");
                pts = timestamp - origin + int64_t(offset_ms) * 1000;
            }
            pts = clock.map(pts, default_duration);
            if (previous != AV_NOPTS_VALUE && pts <= previous) return;
            previous = pts;
            Frame converted = converter.convert(frame.get(), width, height);
            converted->pts = pts;
            converted->duration = frame->duration > 0
                ? av_rescale_q(frame->duration, stream->time_base, AVRational{1, AV_TIME_BASE})
                : default_duration;
            if (converted->duration <= 0 || converted->duration > AV_TIME_BASE)
                converted->duration = default_duration;
            // 移动所有权到跨线程队列；队列满会等待，stop关闭队列可解除此等待。
            queue_.push(std::move(converted));
        };
        Packet packet = make_packet();
        int64_t progress = av_gettime_relative();
        while (!stop_) {
            const int result = input.read(packet.get());
            if (result == AVERROR_EOF) {
                if (is_network_input(url)) throw std::runtime_error("网络视频源结束或断流");
                decoder.send(nullptr, consume);
                break;
            }
            if (result == AVERROR(EAGAIN)) {
                if (av_gettime_relative() - progress > int64_t(timeout_ms) * 1000)
                    throw std::runtime_error("视频源持续 EAGAIN 超时");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            progress = av_gettime_relative();
            if (packet->stream_index == index) {
                if (packet->flags & AV_PKT_FLAG_CORRUPT) throw std::runtime_error("视频源压缩包损坏");
                decoder.send(packet.get(), consume);
            }
        }
        log.log(LogLevel::Info, "视频时间轴重建=" + std::to_string(clock.resets()) +
                    "，缺失PTS=" + std::to_string(clock.missing()));
    } catch (const std::exception& error) {
        if (!stop_) {
            failed_.store(true);
            log.log(LogLevel::Warning, std::string("一路视频源失败：") + error.what());
        }
    }
    queue_.close();
}
}
