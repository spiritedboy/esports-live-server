/*
 * 初学者导读
 * 这是解复用层：从文件或网络容器中拆出各轨道的压缩包，不在这里解码。
 * open 打开 URL 并探测轨道信息；同一个实例只应打开一次。
 * read 把下一包放入调用者提供的 AVPacket，先清理其中上一次读到的引用。
 * 包的 stream_index 标识所属轨道；包 PTS/DTS 的单位要去对应 AVStream 查 time_base。
 * 返回 EOF 表示读完，EAGAIN 表示目前读不到；两者由 VideoSource/AudioSource 分别处理。
 * context 和内部 streams 都是借用指针，StreamInput 析构后不能再访问。
 * IoControl 引用必须保持有效；它提供超时和主动停止，防止 join 永久卡在网络读取。
 */
/*
 * 单线程解复用器，context()只返回借用指针，不能由调用者释放。
 */
#ifndef ESPORTS_INPUT_STREAM_INPUT_H
#define ESPORTS_INPUT_STREAM_INPUT_H
#include "common/ffmpeg.h"
#include "common/logger.h"

namespace esports {
class StreamInput {
public:
    explicit StreamInput(IoControl& io) : io_(io) {}
    ~StreamInput();
    StreamInput(const StreamInput&) = delete;
    StreamInput& operator=(const StreamInput&) = delete;
    // 打开并探测 URL，成功后 context()->streams 才能作为解码器参数来源。
    void open(const std::string& url, Logger& log);
    // 返回 FFmpeg 状态：成功、EOF 或 EAGAIN；其他错误抛异常。
    int read(AVPacket* packet);
    // 借出内部上下文供查询轨道；调用者不 free、不并发操作其读取状态。
    AVFormatContext* context() const { return context_; }
private:
    // 借用控制器；context_ 是本对象拥有的输入上下文，由析构 close_input 释放。
    IoControl& io_;
    AVFormatContext* context_ = nullptr;
};
} // namespace esports
#endif
