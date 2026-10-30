#ifndef ESPORTS_MIXER_AUDIO_MIXER_H
#define ESPORTS_MIXER_AUDIO_MIXER_H
#include "codec/media_codec.h"
#include <vector>
namespace esports {
Frame audio_frame(int samples, int64_t pts);
class AudioMixer {
public:
    explicit AudioMixer(size_t sources);
    void set_gain(size_t index, double gain);
    void set_muted(size_t index, bool muted);
    Frame mix(const std::vector<const AVFrame*>& frames, int64_t pts, int samples);
    int64_t limited_samples() const { return limited_; }
private:
    struct Gain { double value = 1, current = 0, target = 1, step = 0; int remaining = 240; bool muted = false; };
    void update(size_t index);
    std::vector<Gain> gains_;
    double limiter_ = 1;
    int64_t limited_ = 0;
};
}
#endif
