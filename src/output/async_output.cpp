#include "output/async_output.h"
#include <pthread.h>
#include <chrono>
namespace esports {
namespace {
class FfmpegSink : public PacketSink {
public:
    explicit FfmpegSink(IoControl& io) : output_(io, true) {}
    void open(const std::string& url, AVFormatContext* description, Logger& log) override {
        output_.open(url, description, log);
    }
    void write(AVPacket* packet, AVRational time_base, int64_t origin_us) override {
        output_.write(packet, time_base, origin_us);
    }
    void finish() override { output_.finish(); }
private:
    StreamOutput output_;
};
}
AsyncOutput::AsyncOutput(const OutputOptions& options, bool network, Logger& log, SinkFactory factory)
    : options_(options), network_(network), log_(log), factory_(std::move(factory)),
      queue_(options.queue, network) {
    if (options.timeout_ms < 100 || options.reconnect_attempts < 0 || options.backoff_ms < 1)
        throw std::invalid_argument("输出超时/重试参数无效");
    if (!factory_) factory_ = [](IoControl& io) {
        return std::unique_ptr<PacketSink>(new FfmpegSink(io));
    };
}
AsyncOutput::~AsyncOutput() {
    cancel();
    if (worker_.joinable()) worker_.join();
    avformat_free_context(description_);
}
void AsyncOutput::open(const std::string& url, const std::vector<AVCodecContext*>& codecs) {
    if (description_) throw std::logic_error("异步输出已打开");
    description_ = avformat_alloc_context();
    if (!description_) throw std::bad_alloc();
    for (const auto* codec : codecs) {
        AVStream* stream = avformat_new_stream(description_, nullptr);
        if (!stream) throw std::bad_alloc();
        ff_check(avcodec_parameters_from_context(stream->codecpar, codec), "快照输出编码参数");
        stream->time_base = codec->time_base;
    }
    if (description_->nb_streams != 2 ||
        description_->streams[0]->codecpar->codec_id != AV_CODEC_ID_H264 ||
        description_->streams[1]->codecpar->codec_id != AV_CODEC_ID_AAC)
        throw std::runtime_error("异步输出要求 H.264 视频0 + AAC 音频1");
    started_us_ = av_gettime_relative();
    sigset_t blocked, previous;
    sigemptyset(&blocked); sigaddset(&blocked, SIGINT); sigaddset(&blocked, SIGTERM);
    const int result = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (result) ff_check(AVERROR(result), "设置输出线程信号掩码");
    try { worker_ = std::thread(&AsyncOutput::run, this, url); }
    catch (...) { pthread_sigmask(SIG_SETMASK, &previous, nullptr); throw; }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}
void AsyncOutput::check() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_) std::rethrow_exception(error_);
}
void AsyncOutput::submit(Packet packet, AVRational time_base) {
    check();
    if (!worker_.joinable() || finished_) throw std::logic_error("异步输出未运行");
    if (!packet || packet->size < 0 || packet->stream_index < 0 || packet->stream_index > 1 ||
        packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE ||
        time_base.num <= 0 || time_base.den <= 0)
        throw std::runtime_error("无效编码包");
    QueuedPacket item;
    item.dts_us = av_rescale_q(packet->dts, time_base, AVRational{1, AV_TIME_BASE});
    if (item.dts_us < -(int64_t(1) << 50) || item.dts_us > (int64_t(1) << 50))
        throw std::runtime_error("编码包时间戳越界");
    item.bytes = packet->size;
    for (int i = 0; i < packet->side_data_elems; ++i) {
        if (packet->side_data[i].size > options_.queue.bytes - std::min(item.bytes, options_.queue.bytes))
            throw std::runtime_error("编码包附加数据超出队列容量");
        item.bytes += packet->side_data[i].size;
    }
    item.video = packet->stream_index == 0;
    item.time_base = time_base;
    item.queued_us = av_gettime_relative();
    item.packet = std::move(packet);
    if (!queue_.push(std::move(item))) {
        check();
        throw std::runtime_error("输出队列已关闭");
    }
    check();
}
void AsyncOutput::cancel() {
    stop_.store(true);
    queue_.close(true);
    wake_.notify_all();
}
void AsyncOutput::finish(const volatile std::sig_atomic_t* user_stop) {
    if (finished_) { check(); return; }
    finished_ = true;
    queue_.close();
    if (worker_.joinable()) {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (ended_us_) break;
                wake_.wait_for(lock, std::chrono::milliseconds(10));
            }
            if (user_stop && *user_stop) cancel();
        }
        worker_.join();
    }
    check();
}
OutputStats AsyncOutput::snapshot() const {
    OutputStats result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = stats_;
        result.elapsed_us = started_us_ ? (ended_us_ ? ended_us_ : av_gettime_relative()) - started_us_ : 0;
    }
    result.queue = queue_.snapshot();
    return result;
}
void AsyncOutput::run(std::string url) {
    IoControl io(stop_, options_.timeout_ms);
    std::unique_ptr<PacketSink> sink;
    uint64_t session_epoch = 0;
    int64_t origin = 0;
    unsigned retries = 0;
    bool recovering = false, session_started = false;
    const auto recover = [&](const std::string& reason) {
        sink.reset();
        if (!network_ || retries >= static_cast<unsigned>(options_.reconnect_attempts))
            throw std::runtime_error(reason + "；输出恢复次数已耗尽");
        ++retries;
        { std::lock_guard<std::mutex> lock(mutex_); stats_.retries = retries; }
        queue_.reset();
        recovering = true;
        const int delay = std::min(4000, options_.backoff_ms * (1 << std::min(4u, retries - 1)));
        log_.log(LogLevel::Warning, "输出恢复：" + reason + "；等待 " +
            std::to_string(delay) + "ms 后从新 IDR 重连，重试 " + std::to_string(retries));
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(delay), [&] { return stop_.load(); });
        lock.unlock();
        queue_.reset(); // 退避结束后再请求新 IDR，不能发送等待期间的旧缓存。
    };
    try {
        QueuedPacket item;
        while (!stop_ && queue_.pop(item)) {
            if (!queue_.current(item.epoch)) { queue_.dropped(item); continue; }
            const int64_t age = av_gettime_relative() - item.queued_us;
            if (network_ && age > int64_t(options_.queue.latency_ms) * 1000) {
                queue_.dropped(item);
                if (sink && session_started) recover("输出排队超过延迟预算");
                else { queue_.reset(); recovering = true; }
                continue;
            }
            if (sink && session_started && item.epoch != session_epoch) {
                queue_.dropped(item);
                recover("积压缓存已失效，需要清除旧连接缓冲");
                continue;
            }
            if (!sink && network_ && (!item.video || !(item.packet->flags & AV_PKT_FLAG_KEY))) {
                queue_.dropped(item);
                continue;
            }
            if (!sink) {
                try {
                    sink = factory_(io);
                    if (!sink) throw std::runtime_error("输出工厂返回空对象");
                    { std::lock_guard<std::mutex> lock(mutex_); ++stats_.attempts; }
                    sink->open(url, description_, log_);
                    session_epoch = item.epoch;
                    origin = network_ ? item.dts_us : 0;
                    { std::lock_guard<std::mutex> lock(mutex_); ++stats_.sessions; }
                    session_started = false;
                    recovering = false;
                    log_.log(LogLevel::Info, "输出会话已建立，重新写入媒体头");
                } catch (const std::exception& error) {
                    if (stop_) break;
                    queue_.dropped(item);
                    { std::lock_guard<std::mutex> lock(mutex_); ++stats_.failures; }
                    recover(error.what());
                    continue;
                }
            }
            // 握手期间缓存可能过期。尚未发送媒体的新连接可等待新 IDR，不必再次握手。
            if (!queue_.current(item.epoch)) {
                queue_.dropped(item);
                if (session_started) recover("写入前缓存 epoch 已改变");
                else recovering = true;
                continue;
            }
            if (network_ && !session_started) {
                if (!item.video || !(item.packet->flags & AV_PKT_FLAG_KEY)) {
                    queue_.dropped(item); continue;
                }
                if (av_gettime_relative() - item.queued_us > int64_t(options_.queue.latency_ms) * 1000) {
                    queue_.dropped(item); queue_.reset(); recovering = true; continue;
                }
                session_epoch = item.epoch;
                origin = item.dts_us;
            }
            if (network_ && item.dts_us < origin) { queue_.dropped(item); continue; }
            const int64_t writing = av_gettime_relative();
            try {
                sink->write(item.packet.get(), item.time_base, origin);
            } catch (const std::exception& error) {
                if (stop_) break;
                queue_.dropped(item); // FFmpeg可能已消耗引用，绝不重试同一个包。
                { std::lock_guard<std::mutex> lock(mutex_); ++stats_.failures;
                  stats_.max_write_us = std::max(stats_.max_write_us, av_gettime_relative() - writing); }
                recover(error.what());
                continue;
            }
            session_started = true;
            recovering = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (item.video) ++stats_.video_packets; else ++stats_.audio_packets;
                stats_.encoded_bytes += item.bytes;
                stats_.max_queue_us = std::max(stats_.max_queue_us, writing - item.queued_us);
                stats_.max_write_us = std::max(stats_.max_write_us, av_gettime_relative() - writing);
            }
        }
        if (!stop_) {
            if (!sink || recovering || !session_started)
                throw std::runtime_error("输入结束时输出尚未恢复，不能声明推流成功");
            sink->finish();
        } else if (sink) {
            try { sink->finish(); }
            catch (const std::exception& error) {
                log_.log(LogLevel::Warning, std::string("停止时写尾失败：") + error.what());
            }
        }
    } catch (...) {
        if (!stop_) {
            { std::lock_guard<std::mutex> lock(mutex_); error_ = std::current_exception(); }
            queue_.close(true);
        }
    }
    sink.reset();
    { std::lock_guard<std::mutex> lock(mutex_); ended_us_ = av_gettime_relative(); }
    wake_.notify_all();
}
}
