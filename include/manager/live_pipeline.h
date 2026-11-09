/*
 * 初学者导读
 * LiveOptions 是启动时确定的配置快照；主线程校验并补齐逐路数组后才调用 run_live。
 * inputs/video_offsets 一一对应，audio_inputs/gains/muted/audio_offsets 一一对应。
 * 各队列容量单位不同：video_queue_frames 数帧，audio_queue_blocks 数 PCM 块，
 * output_options.queue 还同时限制编码包数量、字节数和网络媒体跨度。
 * realtime 控制按墙钟节奏处理；离线文件模式可以尽快处理，RTMP 必须实时发送。
 * run_live 在调用线程完成调度、合成、混音和编码，并管理其他工作线程的退出。
 * stop 是借用的信号标志：正常结束返回 0，用户停止返回 130，其他失败可能抛异常。
 */
/*
 * 完整节目配置：包含视频合成、独立音频、输出恢复、录制及监控。
 * 入口校验后各逐路数组与最终源列表对齐；不要在运行期间并发修改这些选项。
 */
#ifndef ESPORTS_MANAGER_LIVE_PIPELINE_H
#define ESPORTS_MANAGER_LIVE_PIPELINE_H
#include "common/logger.h"
#include <vector>
#include "output/async_output.h"
#include "recording/recorder.h"
namespace esports {
struct LiveOptions {
    // 视频源列表由入口限制为 2 或 4 路；网络 URL 和本地普通文件共用同一输入抽象。
    std::vector<std::string> inputs;
    std::string output;
    bool realtime = true;
    int timeout_ms = 5000;
    int width = 640, height = 360, fps = 25;
    std::string layout = "split", switch_layout;
    int switch_at_ms = -1; // -1：不安排布局切换。
    int duration_ms = 0; // 0：运行至全部源结束或用户停止。
    // 上一画面距当前节目时刻超过该阈值后改用黑帧，避免断流画面无限冻结。
    int stale_ms = 500;
    // 音频可以独立于视频列表，gains/muted/audio_offsets按音频索引，video_offsets按视频索引。
    std::vector<std::string> audio_inputs;
    std::vector<double> gains;
    std::vector<bool> muted;
    // 偏移单位毫秒：正数延后，负数前移；不是直接修改接收网络包的时间。
    std::vector<int> audio_offsets, video_offsets;
    // 一次预设音量调整：-1 禁用；达到该节目毫秒时刻后修改 change_index 对应音轨。
    int change_at_ms = -1;
    size_t change_index = 0;
    double change_gain = 1;
    // stats_file 观察媒体进度/同步与输出；metrics_file 观察 CPU/内存/处理耗时。空串禁用。
    std::string stats_file, metrics_file;
    OutputOptions output_options;
    RecordingOptions recording;
    // 视频码率为bit/s，GOP/VBV为毫秒；不是包队列长度，也不是端到端延迟保证。
    int video_bitrate = 1200000, gop_ms = 1000, vbv_ms = 500;
    size_t video_queue_frames = 4, audio_queue_blocks = 16;
};
// 调用前要求逐路数组与源列表长度一致。此函数阻塞运行到媒体结束、停止或失败。
int run_live(const LiveOptions&, const volatile std::sig_atomic_t&, Logger&);
}
#endif
