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
    bool stopped() const {
        return (signal_stop_ && *signal_stop_ != 0) ||
               (thread_stop_ && thread_stop_->load(std::memory_order_relaxed));
    }
    bool expired() const { return av_gettime_relative() >= deadline_; }
    AVIOInterruptCB callback() { return AVIOInterruptCB{interrupt, this}; }
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
    const volatile std::sig_atomic_t* signal_stop_ = nullptr;
    const std::atomic<bool>* thread_stop_ = nullptr;
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
inline bool is_network_input(const std::string& url) {
    return starts_with(url, "rtmp://") || starts_with(url, "rtsp://");
}
} // namespace esports
#endif
