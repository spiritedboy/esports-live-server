/*
 * 初学者导读
 * 阅读入口：这里把 FFmpeg 的 C 接口包装成项目统一的资源和错误规则。
 * AVPacket 保存压缩数据（H.264/AAC），不是可以直接显示或播放的原始像素/PCM。
 * Packet 是唯一拥有 AVPacket 外壳的智能指针；get() 借用，move 转交，reset 释放。
 * av_packet_ref/clone 则可以另建外壳并共享引用计数缓冲，二者不要混淆。
 * IoControl 由使用 I/O 的线程独占；停止标志属于外部对象，必须比它活得久。
 * FFmpeg 在阻塞操作中询问 interrupt 回调；回调返回 1 表示中断，不在回调里抛异常。
 * NetworkRuntime 应先构造、后析构，使网络资源覆盖输入和输出会话的生命期。
 */
/*
 * FFmpeg公共边界：错误转异常、AVPacket的RAII、可中断I/O及网络运行期。
 * Packet拥有结构体及其缓冲引用；移动转移所有权，底层编码数据可由多个引用共享。
 */
#ifndef ESPORTS_COMMON_FFMPEG_H
#define ESPORTS_COMMON_FFMPEG_H
#include <csignal>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

namespace esports {
// 统一错误出口：FFmpeg 通常以负数表示失败，非负值可能是成功或有效数量。
// operation 是调用方提供的上下文，和 av_strerror 的底层原因拼成异常。
inline void ff_check(int result, const std::string& operation) {
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(result, error, sizeof(error));
    throw std::runtime_error(operation + "：" + error);
}

// av_packet_free同时unref缓冲并释放外壳；只想复用外壳时使用av_packet_unref。
struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
typedef std::unique_ptr<AVPacket, PacketDeleter> Packet;
// 仅分配压缩包外壳，data/size 尚无媒体内容。后续由 read/receive 或 av_new_packet 填充。
// 分配失败立即抛出 bad_alloc，不让空指针流入下游。
inline Packet make_packet() {
    Packet packet(av_packet_alloc());
    if (!packet) throw std::bad_alloc();
    return packet;
}

// 只在执行媒体循环的同一线程使用。信号处理器仅修改 sig_atomic_t。
class IoControl {
public:
    IoControl(const volatile std::sig_atomic_t& stop, int timeout_ms)
        : signal_stop_(&stop), timeout_us_(static_cast<int64_t>(timeout_ms) * 1000) {}
    IoControl(const std::atomic<bool>& stop, int timeout_ms)
        : thread_stop_(&stop), timeout_us_(static_cast<int64_t>(timeout_ms) * 1000) {}
    // 每次阻塞调用前重新设截止时间。写尾模式忽略停止标志，但仍保留超时。
    // 回调借用this，IoControl必须覆盖整个FFmpeg I/O对象的生命周期。
    void arm(bool finishing = false) {
        finishing_ = finishing;
        deadline_ = av_gettime_relative() + timeout_us_;
    }
    // 两种停止来源二选一：main 的信号标志，或工作线程使用的 atomic<bool>。
    // 这里只需要读取消位，不通过该原子发布其他数据，故线程标志可用 relaxed 读取。
    bool stopped() const {
        return (signal_stop_ && *signal_stop_ != 0) ||
               (thread_stop_ && thread_stop_->load(std::memory_order_relaxed));
    }
    // deadline_ 是单调时钟的绝对微秒值；首次检查前必须先 arm。
    bool expired() const { return av_gettime_relative() >= deadline_; }
    // 把静态 C 风格函数和 this 配成一对；FFmpeg 通过 opaque 找回当前控制对象。
    AVIOInterruptCB callback() { return AVIOInterruptCB{interrupt, this}; }
    // 成功不抛异常；失败优先解释成用户停止或超时，剩余错误交给 ff_check。
    // finishing 模式仍检查超时，但不把已收到的退出信号当作写尾失败原因。
    void check(int result, const std::string& operation) const {
        if (result >= 0) return;
        if (!finishing_ && stopped()) throw std::runtime_error(operation + "：用户停止");
        if (expired()) throw std::runtime_error(operation + "：I/O 超时");
        ff_check(result, operation);
    }
private:
    static int interrupt(void* opaque) {
        const IoControl& self = *static_cast<IoControl*>(opaque);
        return ((!self.finishing_ && self.stopped()) || self.expired()) ? 1 : 0;
    }
    // 以下两个指针都是借用，不负责释放；实际 I/O 使用期间不能销毁对应停止标志。
    const volatile std::sig_atomic_t* signal_stop_ = nullptr;
    const std::atomic<bool>* thread_stop_ = nullptr;
    // timeout_us_ 是每次操作允许的时长；deadline_ 是 arm 算出的本次截止点。
    int64_t timeout_us_;
    int64_t deadline_ = 0;
    bool finishing_ = false;
};

class NetworkRuntime {
public:
    NetworkRuntime() { ff_check(avformat_network_init(), "网络初始化"); }
    ~NetworkRuntime() { avformat_network_deinit(); }
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

inline bool starts_with(const std::string& value, const std::string& prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}
// 这是项目允许的输入协议白名单判断，不是判断任意字符串是否为合法 URL。
inline bool is_network_input(const std::string& url) {
    return starts_with(url, "rtmp://") || starts_with(url, "rtsp://");
}
} // namespace esports
#endif
