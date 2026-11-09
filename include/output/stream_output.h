/*
 * 初学者导读
 * StreamOutput 代表一个封装会话：open 写头，write 送编码包，finish 写尾，析构释放。
 * input 参数只用于复制轨道描述，write 的 input_time_base 才解释每个包的时间戳。
 * mapping_ 把输入流序号映射到输出序号；未选择的轨道为 -1，不能写入。
 * last_dts_ 按输出轨道保存上一个 DTS；同一轨道必须严格递增，不要求两轨互相递增。
 * origin_us 是两轨共用的媒体原点，先换到各输入时间基再同时减 PTS/DTS。
 * 本地文件由 fd_ 和自定义 AVIO 管理；网络由 FFmpeg 的协议 AVIO 管理。
 * write 借用 AVPacket 外壳，但 FFmpeg 写入会消耗缓冲引用，失败也不能原包重试。
 * finish 可以报告写尾/刷新失败；析构 close 不抛异常，因此不能只靠析构认定文件成功。
 */
/*
 * 单个输出会话的封装器；复制流描述后不依赖原描述对象存活。
 * finish用于报告写尾错误，析构只释放资源；调用者应显式完成finish。
 */
#ifndef ESPORTS_OUTPUT_STREAM_OUTPUT_H
#define ESPORTS_OUTPUT_STREAM_OUTPUT_H
#include "common/ffmpeg.h"
#include "common/logger.h"
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}

namespace esports {
class StreamOutput {
public:
    explicit StreamOutput(IoControl& io, bool low_latency = false, const std::string& format = "flv")
        : io_(io), low_latency_(low_latency), format_(format) {}
    ~StreamOutput();
    StreamOutput(const StreamOutput&) = delete;
    StreamOutput& operator=(const StreamOutput&) = delete;
    // 复制 input 的 H.264/AAC 轨道信息并写容器头；本地路径禁止覆盖已有文件。
    void open(const std::string& url, AVFormatContext* input, Logger& log);
    // 查询输入索引是否被映射到输出轨道，回放工具可据此跳过不需要的包。
    bool selected(int input_index) const;
    // packet 归调用者持有；写入会消耗其缓冲引用，失败也不能重用原包。
    void write(AVPacket* packet, AVRational input_time_base, int64_t origin_us);
    // 显式完成写尾和刷新，失败抛异常；幂等，不会因析构再次调用而重复写尾。
    void finish();
private:
    static int write_file(void* opaque, uint8_t* buffer, int size);
    static int64_t seek_file(void* opaque, int64_t offset, int whence);
    // 析构使用的兜底释放路径；不写媒体尾，不把资源释放成功当成录制完成。
    void close() noexcept;
    IoControl& io_;
    bool low_latency_;
    std::string format_;
    AVFormatContext* context_ = nullptr;
    std::vector<int> mapping_;
    std::vector<int64_t> last_dts_;
    int fd_ = -1;
    bool custom_io_ = false;
    bool header_written_ = false;
    bool finished_ = false;
};
} // namespace esports
#endif
