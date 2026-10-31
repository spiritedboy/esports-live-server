/*
 * 完整节目配置：沿用视频合成参数，增加独立音频、输出恢复、录制及监控。
 * 入口校验后各逐路数组与最终源列表对齐；不要在运行期间并发修改这些选项。
 */
#ifndef ESPORTS_MANAGER_LIVE_PIPELINE_H
#define ESPORTS_MANAGER_LIVE_PIPELINE_H
#include "manager/compositor.h"
#include "output/async_output.h"
#include "recording/recorder.h"
namespace esports {
struct LiveOptions : CompositeOptions {
    // 音频可以独立于视频列表，gains/muted/audio_offsets按音频索引，video_offsets按视频索引。
    std::vector<std::string> audio_inputs;
    std::vector<double> gains;
    std::vector<bool> muted;
    // 偏移单位毫秒：正数延后，负数前移；不是直接修改接收网络包的时间。
    std::vector<int> audio_offsets, video_offsets;
    int change_at_ms = -1;
    size_t change_index = 0;
    double change_gain = 1;
    std::string stats_file, metrics_file;
    OutputOptions output_options;
    RecordingOptions recording;
    // 视频码率为bit/s，GOP/VBV为毫秒；不是包队列长度，也不是端到端延迟保证。
    int video_bitrate = 1200000, gop_ms = 1000, vbv_ms = 500;
    size_t video_queue_frames = 4, audio_queue_blocks = 16;
};
int run_live(const LiveOptions&, const volatile std::sig_atomic_t&, Logger&);
}
#endif
