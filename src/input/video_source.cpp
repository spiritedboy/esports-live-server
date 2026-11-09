/*
 * 初学者导读
 * 阅读 run 的四段：选择唯一视频轨 → 建立容器原点与源时钟 → 解码回调处理帧 → 读取包。
 * 解码回调里的 frame 由当前回调拥有；转换后把 converted 移到队列，原 frame 自动释放。
 * 原始 PTS 用 stream->time_base；入队前统一换成微秒并减共同原点、加配置偏移。
 * 例如 time_base=1/90000、PTS=9000 是 100ms；offset_ms=50 后节目位置为 150ms（原点为零）。
 * SourceClock 用默认帧时长修补异常 PTS；duration 则用于估算最后一张画面覆盖到哪里。
 * 读取普通文件的 EOF 会排空解码器，网络 EOF 当作断流；失败后主线程负责冻结/黑帧降级。
 * progress 限制连续 EAGAIN 的总等待，避免每次 read 很快返回却永久没有媒体的情况。
 */
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
// 两种阻塞都要解除：stop_ 让 FFmpeg 中断读，queue_.close 让满队列上的 push 返回。
void VideoSource::stop() { stop_.store(true); queue_.close(); }
// 只在主线程回收工作线程，不能从该 worker 自己调用 join。
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
        // 循环复用一个包外壳；StreamInput::read 在填入新包前 unref 上一包。
        // 解码器 send 只借用该外壳，并按 FFmpeg 规则保存仍需使用的缓冲引用。
        Packet packet = make_packet();
        int64_t progress = av_gettime_relative();
        while (!stop_) {
            const int result = input.read(packet.get());
            if (result == AVERROR_EOF) {
                if (is_network_input(url)) throw std::runtime_error("网络视频源结束或断流");
                decoder.send(nullptr, consume);
                break;
            }
            // 暂时无数据时短睡眠避免空转；progress 不在这里重置，所以总等待仍有上限。
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
    // 无论正常 EOF、异常或主动停止，都通知消费者不会再有新数据，避免消费者无限等待。
    queue_.close();
}
}
