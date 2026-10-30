#ifndef ESPORTS_MANAGER_LIVE_PIPELINE_H
#define ESPORTS_MANAGER_LIVE_PIPELINE_H
#include "manager/compositor.h"
#include "output/async_output.h"
#include "recording/recorder.h"
namespace esports {
struct LiveOptions : CompositeOptions {
    std::vector<std::string> audio_inputs;
    std::vector<double> gains;
    std::vector<bool> muted;
    std::vector<int> audio_offsets, video_offsets;
    int change_at_ms = -1;
    size_t change_index = 0;
    double change_gain = 1;
    std::string stats_file, metrics_file;
    OutputOptions output_options;
    RecordingOptions recording;
    int video_bitrate = 1200000, gop_ms = 1000, vbv_ms = 500;
    size_t video_queue_frames = 4, audio_queue_blocks = 16;
};
int run_live(const LiveOptions&, const volatile std::sig_atomic_t&, Logger&);
}
#endif
