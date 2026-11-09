/*
 * 初学者导读
 * 测试使用已知常量 PCM，便于算出期望幅度；第 1000 个采样已经越过 240 采样的增益过渡。
 * 漂移测试模拟 600 秒媒体，不实际睡眠 600 秒；±1000ppm 表示源时钟每秒偏 ±1ms。
 * 回调检查每个输出块起点等于上一块终点，验证补偿改变采样数时仍保持连续 PTS。
 * 后续主动输入缺失、重复、回退和大跳变 PTS，验证修复计数与不重叠输出。
 * 最后验证窗口部分覆盖时保留静音，以及关闭满队列能唤醒阻塞生产者。
 */
// 用确定的PCM检验增益、限幅、采样窗口和时钟修复；600秒是加速媒体时间，不是实际等待600秒。
#include "mixer/audio_mixer.h"
#include "sync/audio_resampler.h"
#include "queue/audio_queue.h"
#include <cmath>
#include <iostream>
#include <thread>
#include <atomic>
namespace {
// 按 FLTP 两个平面分别填常量；value 表示线性采样幅度，不能用 memset 的字节值替代。
void fill(AVFrame* frame, float value) {
    for (int c = 0; c < 2; ++c)
        std::fill(reinterpret_cast<float*>(frame->extended_data[c]),
                  reinterpret_cast<float*>(frame->extended_data[c]) + frame->nb_samples, value);
}
}
int main() {
    using namespace esports;
    AudioMixer mixer(2);
    Frame a = audio_frame(1024, 0), b = audio_frame(1024, 0);
    fill(a.get(), .25f); fill(b.get(), .5f);
    mixer.set_gain(0, .5); mixer.set_muted(1, true);
    Frame mixed = mixer.mix({a.get(), b.get()}, 0, 1024);
    if (std::abs(reinterpret_cast<float*>(mixed->data[0])[1000] - .125f) > 1e-6) return 1;
    mixer.set_muted(1, false); mixer.set_gain(0, 4); mixer.set_gain(1, 4);
    mixed = mixer.mix({a.get(), b.get()}, 0, 1024);
    for (int i = 0; i < mixed->nb_samples; ++i)
        if (std::abs(reinterpret_cast<float*>(mixed->data[0])[i]) > .95001f) return 2;
    if (!mixer.limited_samples()) return 3;

    // 600 秒媒体时间，正负 1000ppm（每秒 ±1ms）漂移，不能让相位误差无限增长。
    for (int sign : {-1, 1}) {
        AudioResampler resampler;
        Frame input = audio_frame(1024, 0);
        int64_t end = 0, previous = AV_NOPTS_VALUE;
        const int blocks = 600 * 48000 / 1024;
        const FrameConsumer consume = [&](Frame frame) {
            if (previous != AV_NOPTS_VALUE && frame->pts != end)
                throw std::runtime_error("补偿产生不连续输出块");
            previous = frame->pts;
            end = frame->pts + frame->nb_samples;
        };
        int64_t last_pts = 0;
        for (int i = 0; i < blocks; ++i) {
            last_pts = static_cast<int64_t>(std::llround(i * 1024.0 * (1 + sign * .001)));
            resampler.send(input.get(), last_pts, consume);
        }
        resampler.finish(consume);
        const AudioSyncStats stats = resampler.stats();
        const int64_t error = std::llabs((last_pts + 1024) - end);
        std::cout << "600秒，漂移=" << sign * 1000 << "ppm，最终误差=" << error
                  << "采样，峰值=" << stats.max_error << "，补偿次数=" << stats.corrections << '\n';
        if (!stats.corrections || stats.gaps || stats.max_error > 240 || error > 160) return 4;
    }
    // 独立实例测试异常，避免前面的十分钟漂移状态影响这里对重建次数的判断。
    AudioResampler broken;
    Frame input = audio_frame(1024, 0);
    int64_t last_end = INT64_MIN;
    const FrameConsumer monotonic = [&](Frame frame) {
        if (frame->pts < last_end) throw std::runtime_error("异常修复后块重叠");
        last_end = frame->pts + frame->nb_samples;
    };
    broken.send(input.get(), 0, monotonic);
    broken.send(input.get(), AV_NOPTS_VALUE, monotonic);
    broken.send(input.get(), 16000, monotonic); // 约300ms缺口，应该留下静音区间。
    broken.send(input.get(), 16000, monotonic); // 重复时间戳也要重建连续时间轴。
    broken.send(input.get(), -48000, monotonic);
    broken.send(input.get(), 48000 * 30, monotonic);
    broken.finish(monotonic);
    const auto abnormal = broken.stats();
    if (!abnormal.missing || abnormal.resets < 3 || !abnormal.gaps) return 5;

    // 窗口[0,1024)只有[240,720)有输入；其余位置必须保留初始化的静音。
    AudioQueue queue;
    Frame first = audio_frame(480, 240);
    fill(first.get(), .5f);
    queue.push(std::move(first)); queue.close();
    Frame window = audio_frame(1024, 0);
    int filled = 0;
    if (!queue.read_window(window.get(), filled, 0) || filled != 480) return 6;
    const float* pcm = reinterpret_cast<float*>(window->data[0]);
    if (pcm[239] != 0 || pcm[240] != .5f || pcm[719] != .5f || pcm[720] != 0) return 7;
    // 此处使用 AudioQueue 默认 32 块容量，不是 live_pipeline 配置的默认 16 块。
    AudioQueue full;
    for (int i = 0; i < 32; ++i) full.push(audio_frame(1024, i * 1024));
    std::atomic<bool> done(false);
    bool rejected = false;
    std::thread producer([&] { rejected = !full.push(audio_frame(1024, 32768)); done = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool blocked = !done;
    full.close(); producer.join();
    if (!blocked || !rejected || full.peak() != 32) return 8;
    std::cout << "混音增益/静音/限峰、异常时间戳、采样对齐、队列容量及关闭通过\n";
    return 0;
}
