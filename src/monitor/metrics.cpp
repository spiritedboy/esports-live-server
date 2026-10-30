#include "monitor/metrics.h"
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>
namespace esports {
MetricsFile::MetricsFile(const std::string& path) {
    if (path.empty()) return;
    const int fd = ::open(path.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0666);
    if (fd < 0) ff_check(AVERROR(errno), "排他创建性能指标文件");
    file_ = fdopen(fd, "w");
    if (!file_) { ::close(fd); throw std::runtime_error("打开性能指标流失败"); }
    if (std::fprintf(file_, "wall_us,cpu_percent,rss_kib,peak_rss_kib,threads,input_video_fps,output_fps,v0_frames,v1_frames,v2_frames,v3_frames,a0_frames,a1_frames,a2_frames,a3_frames,decode_us,encode_us,processing_us,video_queued,audio_queued,audio_late,output_dropped,skipped_video,skipped_audio\n") < 0) {
        std::fclose(file_); file_ = nullptr;
        throw std::runtime_error("性能指标写头失败");
    }
    started_ = previous_wall_ = av_gettime_relative();
    struct rusage usage = {};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        previous_cpu_ = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * int64_t(1000000) +
                        usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
}
MetricsFile::~MetricsFile() { if (file_) std::fclose(file_); }
void MetricsFile::sample(const PipelineMetrics& data) {
    if (!file_) return;
    const int64_t now = av_gettime_relative(), interval = std::max<int64_t>(1, now - previous_wall_);
    struct rusage usage = {};
    if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("读取进程CPU统计失败");
    const int64_t cpu = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * int64_t(1000000) +
                        usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
    long rss = -1, threads = -1;
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream row(line); std::string key; long value;
        if (row >> key >> value) {
            if (key == "VmRSS:") rss = value;
            if (key == "Threads:") threads = value;
        }
    }
    uint64_t inputs = 0;
    for (auto n : data.video_frames) inputs += n;
    bool bad = std::fprintf(file_, "%lld,%.3f,%ld,%ld,%ld,%.3f,%.3f",
        static_cast<long long>(now-started_), 100.0*(cpu-previous_cpu_)/interval,
        rss, usage.ru_maxrss, threads, 1000000.0*(inputs-previous_input_)/interval,
        1000000.0*(data.output_video-previous_output_)/interval) < 0;
    for (auto n : data.video_frames) bad = (std::fprintf(file_, ",%llu", static_cast<unsigned long long>(n)) < 0) || bad;
    for (auto n : data.audio_frames) bad = (std::fprintf(file_, ",%llu", static_cast<unsigned long long>(n)) < 0) || bad;
    bad = (std::fprintf(file_, ",%llu,%llu,%llu,%zu,%zu,%llu,%llu,%lld,%lld\n",
        static_cast<unsigned long long>(data.decode_us), static_cast<unsigned long long>(data.encode_us),
        static_cast<unsigned long long>(data.processing_us), data.video_queued, data.audio_queued,
        static_cast<unsigned long long>(data.audio_late), static_cast<unsigned long long>(data.output_dropped),
        static_cast<long long>(data.skipped_video), static_cast<long long>(data.skipped_audio)) < 0) || bad;
    if (std::fflush(file_) || bad) throw std::runtime_error("性能指标写入失败");
    previous_wall_ = now; previous_cpu_ = cpu; previous_input_ = inputs; previous_output_ = data.output_video;
}
}
