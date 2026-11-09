/*
 * 初学者导读
 * 录制和推流共享编码结果，但必须使用不同 AVPacket 外壳，因为封装会改时间戳并消耗引用。
 * submit 的主要工作是计量容量和 clone 后入队，实际磁盘 I/O 由 run 在锁外执行。
 * 分段先等关键帧；到目标时长后的下一关键帧成为下一段的第一包，而不是上一段的尾包。
 * 每段封装使用自己的共同时间原点，索引则记录节目时间轴上的 start_us/end_us。
 * finalize 先写媒体尾，再写 TSV 索引；索引中只发布已完成片段，便于 replay 定位。
 * 队列满或写失败可使当前片段不完整；fail 清掉积压并禁用录制，主直播不等待磁盘恢复。
 * 索引 fflush 仅刷新用户态缓冲，这不是断电后文件与索引一致性的事务保证。
 */
/*
 * 编码后独立录制支路，不重复编码。submit克隆包引用，工作线程负责分段封装和索引。
 * 磁盘慢/写失败只关闭录制；直播继续。只有写尾成功的片段才登记索引。
 */
#include "recording/recorder.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
namespace esports {
Recorder::Recorder(const RecordingOptions& options, Logger& log) : options_(options), log_(log) {}
Recorder::~Recorder() { finish(); avformat_free_context(description_); }
// 故障隔离出口。清缓存时 Packet 的 RAII 释放仅减少录制持有的引用，不破坏主输出的引用。
void Recorder::fail(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = closed_ = true;
        queue_.clear(); bytes_ = 0;
    }
    ready_.notify_all();
    log_.log(LogLevel::Error, "录制已停止，直播继续：" + reason);
}
// 将前缀变为绝对路径，让索引在从不同工作目录运行 replay 时仍能找到分段。
void Recorder::open(const std::vector<AVCodecContext*>& codecs) {
    if (options_.prefix.empty()) return;
    try {
        if (options_.prefix[0] != '/') {
            std::unique_ptr<char, void(*)(void*)> directory(::getcwd(nullptr, 0), std::free);
            if (!directory) throw std::runtime_error("无法取得录制工作目录");
            options_.prefix = std::string(directory.get()) + "/" + options_.prefix;
        }
        if (description_) throw std::logic_error("录制不能重复打开");
        description_ = avformat_alloc_context();
        if (!description_) throw std::bad_alloc();
        for (const auto* codec : codecs) {
            AVStream* stream = avformat_new_stream(description_, nullptr);
            if (!stream) throw std::bad_alloc();
            ff_check(avcodec_parameters_from_context(stream->codecpar, codec), "录制参数快照");
            stream->time_base = codec->time_base;
        }
        sigset_t set, old;
        sigemptyset(&set); sigaddset(&set, SIGINT); sigaddset(&set, SIGTERM);
        ff_check(AVERROR(pthread_sigmask(SIG_BLOCK, &set, &old)), "屏蔽录制线程信号");
        try { worker_ = std::thread(&Recorder::run, this); }
        catch (...) { pthread_sigmask(SIG_SETMASK, &old, nullptr); throw; }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
    } catch (const std::exception& e) { fail(e.what()); }
}
// 不等待磁盘写完；但仍短暂加锁保护入队。容量不足时关闭录制，避免把磁盘背压传给直播。
void Recorder::submit(const AVPacket* packet, AVRational tb) {
    if (options_.prefix.empty()) return;
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) return;
        size_t size = packet->size;
        for (int i = 0; i < packet->side_data_elems; ++i) size += packet->side_data[i].size;
        if (queue_.size() >= 1024 || size > options_.queue_bytes - bytes_) {
            lock.unlock(); fail("录制队列满，防止磁盘背压冻结直播"); return;
        }
        QueuedPacket item;
        // 独立AVPacket外壳共享只读编码数据；两路封装会各改PTS/DTS，不能共用同一外壳。
        item.packet.reset(av_packet_clone(packet));
        if (!item.packet) throw std::bad_alloc();
        item.time_base = tb; item.bytes = size;
        item.dts_us = av_rescale_q(packet->dts, tb, AVRational{1, AV_TIME_BASE});
        queue_.push_back(std::move(item)); bytes_ += size;
        ready_.notify_one();
    } catch (const std::exception& e) { fail(e.what()); }
}
void Recorder::finish() {
    { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void Recorder::run() {
    std::atomic<bool> stop(false);
    IoControl io(stop, 3000);
    std::unique_ptr<StreamOutput> mux;
    std::unique_ptr<FILE, int(*)(FILE*)> index(nullptr, std::fclose);
    int64_t origin = 0, end = 0;
    unsigned segment = 0;
    std::string path;
    bool published = false;
    // 先成功写尾再发布索引，防止把未完成的MP4当作可回放片段。
    // 这不提供断电持久性保证；AAC边界也不保证采样级无缝。
    // 只有 mux.finish 成功后才发布索引；写尾失败会跳到 catch，不把坏片段宣布为可回放。
    const auto finalize = [&] {
        if (!mux) return;
        mux->finish();
        if (std::fprintf(index.get(), "%u\t%lld\t%lld\t%s\n", segment,
            static_cast<long long>(origin), static_cast<long long>(end), path.c_str()) < 0 ||
            std::fflush(index.get()) != 0) throw std::runtime_error("录制索引写入失败");
        published = true;
        mux.reset();
    };
    try {
        const int fd = ::open((options_.prefix + ".index.tsv").c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0666);
        if (fd < 0) ff_check(AVERROR(errno), "排他创建录制索引");
        FILE* file = fdopen(fd, "w");
        if (!file) { ::close(fd); throw std::runtime_error("打开索引流失败"); }
        index.reset(file);
        if (std::fprintf(file, "segment\tstart_us\tend_us\tpath\n") < 0 || std::fflush(file))
            throw std::runtime_error("录制索引写头失败");
        for (;;) {
            QueuedPacket item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return closed_ || !queue_.empty(); });
                if (queue_.empty()) break;
                item = std::move(queue_.front()); queue_.pop_front(); bytes_ -= item.bytes;
            }
            // 切段判断用视频关键帧的 DTS；音频包不能触发新段，否则新段可能无法独立解码。
            const bool key = item.packet->stream_index == 0 && (item.packet->flags & AV_PKT_FLAG_KEY);
            // 目标时长到达后等下一IDR才切段，让每段视频能独立解码；实际时长可能更长。
            if (mux && key && item.dts_us - origin >= int64_t(options_.segment_ms) * 1000) {
                end = item.dts_us; // 索引使用互不重叠的半开节目区间。
                finalize(); ++segment;
            }
            if (!mux) {
                if (!key) continue;
                origin = end = item.dts_us;
                char number[32]; std::snprintf(number, sizeof(number), "-%06u.", segment);
                path = options_.prefix + number + (options_.format == "mp4" ? "mp4" : "mkv");
                mux.reset(new StreamOutput(io, false, options_.format));
                mux->open(path, description_, log_);
            }
            // AAC编码延迟可使切段后到达的包早于新IDR，丢弃这部分预卷。
            if (item.dts_us < origin) continue;
            end = std::max(end, item.dts_us + av_rescale_q(item.packet->duration,
                item.time_base, AVRational{1, AV_TIME_BASE}));
            mux->write(item.packet.get(), item.time_base, origin);
        }
        finalize();
        bool failed;
        { std::lock_guard<std::mutex> lock(mutex_); failed = failed_; }
        if (!failed) {
            if (!published) throw std::runtime_error("没有可录制的关键帧");
            log_.log(LogLevel::Info, "录制完成，索引：" + options_.prefix + ".index.tsv");
        }
    } catch (const std::exception& e) { fail(e.what()); }
}
}
