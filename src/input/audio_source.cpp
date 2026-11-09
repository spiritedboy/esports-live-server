/*
 * 初学者导读
 * 音频链路比视频多一个缓冲层：解码器内部可能留帧，重采样器内部可能留采样。
 * 因此正常文件结束时先 decoder.send(nullptr)，再 resampler.finish，顺序不能颠倒。
 * 解码 PTS 先转换为微秒减容器起点，再换成 48kHz 采样位置；正偏移每毫秒增加 48。
 * 缺失 PTS 保留 AV_NOPTS_VALUE 交给 SourceClock 推算，不能拿这个特殊整数做普通减法。
 * resampler 的回调仍在同一音频工作线程，只有 queue_.push 才发生跨线程所有权交接。
 * 工作线程最外层捕获异常，记录失败并关闭队列；不让异常逃出 std::thread 导致进程终止。
 */
/*
 * 一路音频一个工作线程：解复用 → 解码 → 同步重采样 → AudioQueue。
 * 输出统一为48kHz双声道FLTP，PTS单位是采样；同URL的视频仍由另一线程独立打开。
 */
#include "input/audio_source.h"
#include "input/stream_input.h"
#include <pthread.h>
#include <sys/stat.h>
#include <cerrno>
namespace esports {
AudioSource::AudioSource(const std::string& url, int timeout_ms, int offset_ms, Logger& log, size_t queue_blocks) : queue_(queue_blocks) {
    // 新线程继承创建线程的信号掩码；创建后恢复主线程掩码，使退出信号由主线程处理。
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    const int result = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (result) ff_check(AVERROR(result), "设置音频线程信号掩码");
    try {
        worker_ = std::thread(&AudioSource::run, this, url, timeout_ms, offset_ms, std::ref(log));
    } catch (...) {
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        throw;
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}
AudioSource::~AudioSource() { stop(); join(); }
// 两种阻塞都要解除：stop_ 让 FFmpeg 中断读，queue_.close 让满队列上的 push 返回。
void AudioSource::stop() { stop_.store(true); queue_.close(); }
// 只在主线程回收工作线程，不能从该 worker 自己调用 join。
void AudioSource::join() { if (worker_.joinable()) worker_.join(); }
void AudioSource::run(std::string url, int timeout_ms, int offset_ms, Logger& log) {
    AudioResampler resampler;
    try {
        IoControl io(stop_, timeout_ms);
        StreamInput input(io);
        if (!is_network_input(url)) {
            struct stat info = {};
            if (stat(url.c_str(), &info) < 0) ff_check(AVERROR(errno), "检查音频文件");
            if (!S_ISREG(info.st_mode)) throw std::runtime_error("音频源必须为普通文件");
        }
        input.open(url, log);
        AVFormatContext* format = input.context();
        int index = -1;
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                if (index >= 0) throw std::runtime_error("每个音频源仅支持一个音轨");
                index = i;
            }
        }
        if (index < 0) {
            log.log(LogLevel::Info, "源无音轨，按静音处理");
            queue_.close();
            return;
        }
        MediaDecoder decoder(&metrics_);
        AVStream* stream = format->streams[index];
        decoder.open(stream);
        int64_t origin = container_origin_us(format);
        const FrameConsumer decoded = [&](Frame frame) {
            if (stop_) return;
            int64_t pts = AV_NOPTS_VALUE;
            if (frame->pts != AV_NOPTS_VALUE) {
                const int64_t us = av_rescale_q(frame->pts, stream->time_base, AVRational{1, AV_TIME_BASE});
                if (us < -(int64_t(1) << 50) || us > (int64_t(1) << 50))
                    throw std::runtime_error("音频时间戳越界");
                if (origin == AV_NOPTS_VALUE) {
                    origin = us;
                    log.log(LogLevel::Warning, "容器缺少起点，音频使用首帧起点；跨轨偏移无法保证");
                }
                if (origin < -(int64_t(1) << 50) || origin > (int64_t(1) << 50))
                    throw std::runtime_error("音频起点越界");
                // 先减容器共同起点，再换成48kHz采样位置；1ms=48采样，正偏移使声音后移。
                pts = av_rescale_q(us - origin, AVRational{1, AV_TIME_BASE},
                                   AVRational{1, 48000}) + int64_t(offset_ms) * 48;
            }
            resampler.send(frame.get(), pts, [&](Frame output) {
                if (!stop_) queue_.push(std::move(output));
            });
        };
        // 循环复用一个包外壳；StreamInput::read 在填入新包前 unref 上一包。
        // 解码器 send 只借用该外壳，并按 FFmpeg 规则保存仍需使用的缓冲引用。
        Packet packet = make_packet();
        int64_t progress = av_gettime_relative();
        while (!stop_) {
            const int result = input.read(packet.get());
            if (result == AVERROR_EOF) {
                if (is_network_input(url)) throw std::runtime_error("音频网络源结束或断流");
                decoder.send(nullptr, decoded);
                resampler.finish([&](Frame output) {
                    if (!stop_) queue_.push(std::move(output));
                });
                break;
            }
            // 暂时无数据时短睡眠避免空转；progress 不在这里重置，所以总等待仍有上限。
            if (result == AVERROR(EAGAIN)) {
                if (av_gettime_relative() - progress > int64_t(timeout_ms) * 1000)
                    throw std::runtime_error("音频源持续 EAGAIN 超时");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            progress = av_gettime_relative();
            if (packet->stream_index == index) {
                if (packet->flags & AV_PKT_FLAG_CORRUPT) throw std::runtime_error("损坏音频包");
                decoder.send(packet.get(), decoded);
            }
        }
    } catch (const std::exception& error) {
        if (!stop_) log.log(LogLevel::Warning, std::string("一路音频源失败，降级静音：") + error.what());
    }
    const AudioSyncStats stats = resampler.stats();
    log.log(LogLevel::Info, "音频同步：输入相位误差峰值 samples=" + std::to_string(stats.max_error) +
        "，软补偿=" + std::to_string(stats.corrections) + "，间断=" + std::to_string(stats.gaps) +
        "，时间轴重建=" + std::to_string(stats.resets) + "，缺失PTS=" + std::to_string(stats.missing));
    // 无论正常 EOF、异常或主动停止，都通知消费者不会再有新数据，避免消费者无限等待。
    queue_.close();
}
}
