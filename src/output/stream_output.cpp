#include "output/stream_output.h"
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

namespace esports {
StreamOutput::~StreamOutput() { close(); }

int StreamOutput::write_file(void* opaque, uint8_t* buffer, int size) {
    StreamOutput& self = *static_cast<StreamOutput*>(opaque);
    int written = 0;
    while (written < size) {
        const ssize_t result = ::write(self.fd_, buffer + written, size - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return AVERROR(errno);
        }
        if (result == 0) return AVERROR(EIO);
        written += static_cast<int>(result);
    }
    return written;
}

int64_t StreamOutput::seek_file(void* opaque, int64_t offset, int whence) {
    StreamOutput& self = *static_cast<StreamOutput*>(opaque);
    if (whence == AVSEEK_SIZE) {
        struct stat info = {};
        if (fstat(self.fd_, &info) < 0) return AVERROR(errno);
        return info.st_size;
    }
    const off_t result = lseek(self.fd_, offset, whence & ~AVSEEK_FORCE);
    return result < 0 ? AVERROR(errno) : result;
}

void StreamOutput::open_encoded(const std::string& url,
                               const std::vector<AVCodecContext*>& codecs, Logger& log) {
    // 仅存放已打开编码器的流描述，复用现有封装和文件保护逻辑。
    struct FormatDeleter {
        void operator()(AVFormatContext* c) const { avformat_free_context(c); }
    };
    std::unique_ptr<AVFormatContext, FormatDeleter> description(avformat_alloc_context());
    if (!description) throw std::bad_alloc();
    for (const auto* codec : codecs) {
        AVStream* stream = avformat_new_stream(description.get(), nullptr);
        if (!stream) throw std::bad_alloc();
        ff_check(avcodec_parameters_from_context(stream->codecpar, codec), "复制编码参数");
        stream->time_base = codec->time_base;
    }
    open(url, description.get(), log);
}

void StreamOutput::open(const std::string& url, AVFormatContext* input, Logger& log) {
    if (context_) throw std::logic_error("输出已经打开");
    if (format_ != "flv" && format_ != "mp4" && format_ != "matroska")
        throw std::runtime_error("不支持的封装格式");
    ff_check(avformat_alloc_output_context2(&context_, nullptr, format_.c_str(), url.c_str()),
             "创建媒体输出");
    if (!context_) throw std::bad_alloc();
    context_->interrupt_callback = io_.callback();
    context_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
    context_->max_interleave_delta = low_latency_ ? 100000 : 500000; // 微秒；限制封装交织等待的媒体时间跨度。
    if (low_latency_) {
        context_->flags |= AVFMT_FLAG_FLUSH_PACKETS;
        context_->flush_packets = 1;
    }
    mapping_.assign(input->nb_streams, -1);
    bool video = false;
    bool audio = false;
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        AVStream* source = input->streams[i];
        const AVCodecParameters* parameters = source->codecpar;
        if (parameters->codec_type != AVMEDIA_TYPE_VIDEO &&
            parameters->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if ((parameters->codec_type == AVMEDIA_TYPE_VIDEO &&
             parameters->codec_id != AV_CODEC_ID_H264) ||
            (parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
             parameters->codec_id != AV_CODEC_ID_AAC)) {
            throw std::runtime_error("不兼容编码：Phase 1 仅允许 H.264/AAC Remux");
        }
        bool& found = parameters->codec_type == AVMEDIA_TYPE_VIDEO ? video : audio;
        if (found) throw std::runtime_error("Phase 1 每种媒体仅允许一路流");
        found = true;
        if (source->time_base.num <= 0 || source->time_base.den <= 0)
            throw std::runtime_error("输入 time_base 无效");
        AVStream* target = avformat_new_stream(context_, nullptr);
        if (!target) throw std::bad_alloc();
        ff_check(avcodec_parameters_copy(target->codecpar, parameters), "复制流参数");
        target->codecpar->codec_tag = 0;
        target->time_base = source->time_base;
        mapping_[i] = target->index;
        log.log(LogLevel::Info, "流映射 " + std::to_string(i) + " → " +
                std::to_string(target->index));
    }
    if (!video && !audio) throw std::runtime_error("输入没有 H.264/AAC 音视频流");
    last_dts_.assign(context_->nb_streams, AV_NOPTS_VALUE);

    io_.arm();
    if (starts_with(url, "rtmp://")) {
        io_.check(avio_open2(&context_->pb, url.c_str(), AVIO_FLAG_WRITE,
                            &context_->interrupt_callback, nullptr), "打开 RTMP 输出");
    } else {
        // 原子排他创建：包括符号链接在内的已有路径均不覆盖。
        fd_ = ::open(url.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd_ < 0) ff_check(AVERROR(errno), "创建输出文件（禁止覆盖已有文件）");
        const int buffer_size = 32768;
        unsigned char* buffer = static_cast<unsigned char*>(av_malloc(buffer_size));
        if (!buffer) throw std::bad_alloc();
        context_->pb = avio_alloc_context(buffer, buffer_size, 1, this,
                                          nullptr, write_file, seek_file);
        if (!context_->pb) {
            av_free(buffer);
            throw std::bad_alloc();
        }
        custom_io_ = true;
        context_->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    io_.arm();
    AVDictionary* mux_options = nullptr;
    if (starts_with(url, "rtmp://")) {
        const int result = av_dict_set(&mux_options, "flvflags", "no_duration_filesize", 0);
        if (result < 0) { av_dict_free(&mux_options); ff_check(result, "设置直播 FLV 标记"); }
    }
    const int header_result = avformat_write_header(context_, &mux_options);
    av_dict_free(&mux_options);
    io_.check(header_result, "写入媒体头");
    header_written_ = true;
    for (unsigned i = 0; i < context_->nb_streams; ++i) {
        const AVRational tb = context_->streams[i]->time_base;
        log.log(LogLevel::Info, "输出流 " + std::to_string(i) + "，写头后 time_base=" +
                std::to_string(tb.num) + "/" + std::to_string(tb.den));
    }
}

bool StreamOutput::selected(int index) const {
    return index >= 0 && static_cast<size_t>(index) < mapping_.size() && mapping_[index] >= 0;
}

void StreamOutput::write(AVPacket* packet, AVRational input_time_base, int64_t origin_us) {
    if (!selected(packet->stream_index)) throw std::logic_error("未选择的输出流");
    const int index = mapping_[packet->stream_index];
    const AVRational output_time_base = context_->streams[index]->time_base;
    // 使用共同微秒原点换算到各自输入时间基，保持音视频相对偏移。
    const int64_t offset = av_rescale_q(origin_us, AVRational{1, AV_TIME_BASE}, input_time_base);
    const auto subtract = [offset](int64_t value) {
        if ((offset > 0 && value < std::numeric_limits<int64_t>::min() + offset) ||
            (offset < 0 && value > std::numeric_limits<int64_t>::max() + offset))
            throw std::runtime_error("时间戳减法溢出");
        return value - offset;
    };
    packet->pts = subtract(packet->pts);
    packet->dts = subtract(packet->dts);
    av_packet_rescale_ts(packet, input_time_base, output_time_base);
    if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE ||
        packet->pts < packet->dts ||
        (last_dts_[index] != AV_NOPTS_VALUE && packet->dts <= last_dts_[index]))
        throw std::runtime_error("输出时间戳无效或 DTS 不严格递增，停止转推");
    last_dts_[index] = packet->dts;
    packet->stream_index = index;
    packet->pos = -1;
    io_.arm();
    io_.check(av_interleaved_write_frame(context_, packet), "写入媒体包");
    if (context_->pb) {
        if (low_latency_) avio_flush(context_->pb);
        io_.check(context_->pb->error, "输出 I/O");
    }
}

void StreamOutput::finish() {
    if (!header_written_ || finished_) return;
    finished_ = true; // 出错也不在析构期间重复写尾。
    io_.arm(true); // 用户停止后仍允许有超时限制的写尾。
    io_.check(av_write_trailer(context_), "写入媒体尾");
    avio_flush(context_->pb);
    io_.check(context_->pb->error, "刷新输出");
    if (!custom_io_) {
        io_.arm(true);
        io_.check(avio_closep(&context_->pb), "关闭 RTMP 输出");
    } else if (fd_ >= 0) {
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) < 0) ff_check(AVERROR(errno), "关闭输出文件");
    }
}

void StreamOutput::close() noexcept {
    if (context_) {
        if (context_->pb) {
            if (custom_io_) {
                av_freep(&context_->pb->buffer);
                avio_context_free(&context_->pb);
            } else {
                io_.arm(true);
                avio_closep(&context_->pb);
            }
        }
        avformat_free_context(context_);
    }
    if (fd_ >= 0) ::close(fd_);
}
} // namespace esports
