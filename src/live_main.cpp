#include "manager/live_pipeline.h"
#include "config/live_config.h"
#include <set>
#include <iostream>
#include <cmath>
namespace {
volatile std::sig_atomic_t stop_requested = 0;
void stop_handler(int) { stop_requested = 1; }
int integer(const std::string& text, int low, int high) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("参数必须为非负整数");
    const unsigned long value = std::stoul(text);
    if (value < static_cast<unsigned long>(low) || value > static_cast<unsigned long>(high))
        throw std::runtime_error("数值超出允许范围");
    return static_cast<int>(value);
}
void validate_layout(const std::string& layout, size_t count) {
    if (layout != "split" && layout != "pip" && layout != "grid")
        throw std::runtime_error("布局必须是 split、pip 或 grid");
    if (layout == "grid" && count != 4) throw std::runtime_error("grid 需要 4 路输入");
}
}
int main(int argc, char** argv) {
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    std::signal(SIGPIPE, SIG_IGN);
    // 文件大小上限通过write返回EFBIG，让输出/录制支路正常报告错误并释放资源。
    std::signal(SIGXFSZ, SIG_IGN);
    esports::Logger log(std::cerr);
    try {
        esports::LiveOptions options;
        std::vector<std::string> command;
        std::string config_path;
        bool config_seen = false, check_config = false, dump_config = false;
        // 先收集CLI，不依赖--config在参数列表中的位置。
        for (int i = 1; i < argc; ++i) {
            const std::string key(argv[i]);
            if (key == "--config") {
                if (config_seen || ++i == argc) throw std::runtime_error("--config只能指定一次且必须有文件路径");
                config_seen = true; config_path = argv[i];
            } else if (key == "--check-config") check_config = true;
            else if (key == "--dump-config") dump_config = true;
            else {
                command.push_back(key);
                if (key != "--no-realtime" && key != "--realtime" && key != "--help") {
                    if (++i == argc) throw std::runtime_error("参数缺少值：" + key);
                    command.push_back(argv[i]);
                }
            }
        }
        const auto indexed = [](const std::string& key) {
            return key == "--gain" || key == "--audio-offset-ms" || key == "--video-offset-ms";
        };
        std::set<std::string> overrides;
        for (size_t i = 0; i < command.size(); ++i) {
            const auto& key = command[i];
            if (key == "--help") continue;
            if (key == "--realtime" || key == "--no-realtime") { overrides.insert("realtime"); continue; }
            const auto& value = command[++i];
            if (indexed(key)) {
                const auto colon = value.find(':');
                if (colon == std::string::npos) throw std::runtime_error("逐路参数格式为索引:值");
                overrides.insert(key + ":" + std::to_string(integer(value.substr(0, colon), 0, 3)));
            } else overrides.insert(key);
        }
        std::vector<std::string> arguments;
        if (config_seen) {
            const auto configured = esports::load_live_config(config_path);
            for (size_t i = 0; i < configured.size(); ++i) {
                const std::string key = configured[i];
                if (key == "--realtime" || key == "--no-realtime") {
                    if (!overrides.count("realtime")) arguments.push_back(key);
                    continue;
                }
                const std::string value = configured[++i];
                const std::string id = indexed(key) ? key + ":" + value.substr(0, value.find(':')) : key;
                // mute/unmute逐索引顺序应用；输入列表等同名CLI字段完整替换JSON。
                if (key != "--mute" && key != "--unmute" && overrides.count(id)) continue;
                arguments.push_back(key); arguments.push_back(value);
            }
        }
        arguments.insert(arguments.end(), command.begin(), command.end());
        bool has_change = false;
        std::vector<std::pair<size_t, double>> gains;
        std::vector<std::pair<size_t, bool>> muted;
        std::vector<std::pair<size_t, int>> audio_offsets, video_offsets;
        for (size_t i = 0; i < arguments.size(); ++i) {
            const std::string key(arguments[i]);
            if (key == "--help") {
                std::cout << "用法：esports_live [--config config/server.json] [--check-config|--dump-config] --input A --input B [--input C --input D] "
                    "--output 新文件.flv|rtmp://... [--layout split|grid|pip] "
                    "[--width 640 --height 360 --fps 25] [--duration-ms 0] "
                    "[--stale-ms 500] [--timeout-ms 5000] [--no-realtime|--realtime] [--unmute 索引] "
                    "[--switch-at-ms 1000 --switch-layout pip] [--audio-input AUDIO] [--gain 0:0.5] [--mute 1] [--audio-offset-ms 0:100] [--video-offset-ms 0:100] [--change-at-ms 1000 --change-gain 0:0] [--stats-file 新文件.csv] [--metrics-file 性能.csv] [--output-timeout-ms 3000] [--reconnect-attempts 5] [--retry-backoff-ms 250] [--output-latency-ms 500] [--packet-queue-count 256] [--packet-queue-bytes 8388608] [--video-queue-frames 4] [--audio-queue-blocks 16] [--video-bitrate 1200000] [--gop-ms 1000] [--vbv-ms 500] [--record-prefix 路径前缀 --record-format mp4|mkv --segment-ms 60000 --record-queue-bytes 16777216]\n";
                return 0;
            }
            if (key == "--no-realtime") { options.realtime = false; continue; }
            if (key == "--realtime") { options.realtime = true; continue; }
            if (++i == arguments.size()) throw std::runtime_error("参数缺少值");
            const std::string value(arguments[i]);
            if (key == "--input") options.inputs.push_back(value);
            else if (key == "--audio-input") options.audio_inputs.push_back(value);
            else if (key == "--mute" || key == "--unmute") muted.push_back(std::make_pair(integer(value, 0, 3), key == "--mute"));
            else if (key == "--metrics-file") options.metrics_file = value;
            else if (key == "--stats-file") options.stats_file = value;
            else if (key == "--change-at-ms") options.change_at_ms = integer(value, 0, 86400000);
            else if (key == "--gain" || key == "--change-gain" ||
                     key == "--audio-offset-ms" || key == "--video-offset-ms") {
                const size_t colon = value.find(':');
                if (colon == std::string::npos) throw std::runtime_error("逐路参数格式为 索引:值");
                const size_t index = integer(value.substr(0, colon), 0, 3);
                const std::string number = value.substr(colon + 1);
                size_t used = 0;
                if (key == "--gain" || key == "--change-gain") {
                    const double gain = std::stod(number, &used);
                    if (used != number.size() || !std::isfinite(gain) || gain < 0 || gain > 4)
                        throw std::runtime_error("增益必须为 0–4 的有限数");
                    if (key == "--gain") gains.push_back(std::make_pair(index, gain));
                    else { options.change_index = index; options.change_gain = gain; has_change = true; }
                } else {
                    const long offset = std::stol(number, &used);
                    if (used != number.size() || offset < -60000 || offset > 60000)
                        throw std::runtime_error("时间偏移范围 -60000–60000ms");
                    if (key == "--audio-offset-ms") audio_offsets.push_back(std::make_pair(index, offset));
                    else video_offsets.push_back(std::make_pair(index, offset));
                }
            }
            else if (key == "--record-prefix") options.recording.prefix = value;
            else if (key == "--record-format") options.recording.format = value == "mkv" ? "matroska" : value;
            else if (key == "--segment-ms") options.recording.segment_ms = integer(value, 100, 86400000);
            else if (key == "--record-queue-bytes") options.recording.queue_bytes = integer(value, 1024, 67108864);
            else if (key == "--output") options.output = value;
            else if (key == "--layout") options.layout = value;
            else if (key == "--switch-layout") options.switch_layout = value;
            else if (key == "--switch-at-ms") options.switch_at_ms = integer(value, 0, 86400000);
            else if (key == "--duration-ms") options.duration_ms = integer(value, 0, 86400000);
            else if (key == "--stale-ms") options.stale_ms = integer(value, 0, 60000);
            else if (key == "--output-timeout-ms") options.output_options.timeout_ms = integer(value, 100, 300000);
            else if (key == "--reconnect-attempts") options.output_options.reconnect_attempts = integer(value, 0, 100);
            else if (key == "--retry-backoff-ms") options.output_options.backoff_ms = integer(value, 10, 4000);
            else if (key == "--output-latency-ms") options.output_options.queue.latency_ms = integer(value, 50, 10000);
            else if (key == "--packet-queue-count") options.output_options.queue.packets = integer(value, 4, 4096);
            else if (key == "--packet-queue-bytes") options.output_options.queue.bytes = integer(value, 4096, 67108864);
            else if (key == "--video-queue-frames") options.video_queue_frames = integer(value, 1, 64);
            else if (key == "--audio-queue-blocks") options.audio_queue_blocks = integer(value, 1, 128);
            else if (key == "--video-bitrate") options.video_bitrate = integer(value, 64000, 20000000);
            else if (key == "--gop-ms") options.gop_ms = integer(value, 100, 5000);
            else if (key == "--vbv-ms") options.vbv_ms = integer(value, 50, 2000);
            else if (key == "--timeout-ms") options.timeout_ms = integer(value, 100, 300000);
            else if (key == "--width") options.width = integer(value, 16, 3840);
            else if (key == "--height") options.height = integer(value, 16, 2160);
            else if (key == "--fps") options.fps = integer(value, 1, 60);
            else throw std::runtime_error("未知选项：" + key);
        }
        if (options.recording.format != "mp4" && options.recording.format != "matroska")
            throw std::runtime_error("录制格式仅支持mp4或mkv");
        if (options.recording.prefix.find_first_of("\t\r\n:") != std::string::npos)
            throw std::runtime_error("录制前缀必须为不含控制分隔符的本地路径");
        if (options.inputs.size() != 2 && options.inputs.size() != 4)
            throw std::runtime_error("合成需要 2 或 4 路输入");
        if (options.output.empty()) throw std::runtime_error("缺少输出路径");
        if (options.width % 4 || options.height % 4)
            throw std::runtime_error("合成宽高必须是 4 的倍数");
        validate_layout(options.layout, options.inputs.size());
        if ((options.switch_at_ms >= 0) != !options.switch_layout.empty())
            throw std::runtime_error("布局切换需要同时设置时间和目标布局");
        if (!options.switch_layout.empty()) validate_layout(options.switch_layout, options.inputs.size());
        for (const auto& url : options.inputs) {
            if (url.empty() || (!esports::is_network_input(url) && url.find(':') != std::string::npos))
                throw std::runtime_error("输入仅支持本地文件、RTSP 或 RTMP");
        }
        if (!esports::starts_with(options.output, "rtmp://") && options.output.find(':') != std::string::npos)
            throw std::runtime_error("输出仅支持本地 FLV 或 RTMP");
        if (esports::starts_with(options.output, "rtmp://") && !options.realtime)
            throw std::runtime_error("RTMP 低延迟输出要求实时调度，不能使用 --no-realtime");
        if (options.audio_inputs.empty()) options.audio_inputs = options.inputs;
        if (options.audio_inputs.size() > 4) throw std::runtime_error("音频源最多 4 路");
        for (const auto& url : options.audio_inputs)
            if (url.empty() || (!esports::is_network_input(url) && url.find(':') != std::string::npos))
                throw std::runtime_error("音频源协议不支持");
        options.gains.assign(options.audio_inputs.size(), 1);
        options.muted.assign(options.audio_inputs.size(), false);
        options.audio_offsets.assign(options.audio_inputs.size(), 0);
        options.video_offsets.assign(options.inputs.size(), 0);
        for (const auto& entry : gains) options.gains.at(entry.first) = entry.second;
        for (const auto& entry : muted) options.muted.at(entry.first) = entry.second;
        for (const auto& entry : audio_offsets) options.audio_offsets.at(entry.first) = entry.second;
        for (const auto& entry : video_offsets) options.video_offsets.at(entry.first) = entry.second;
        if (has_change != (options.change_at_ms >= 0) || options.change_index >= options.audio_inputs.size())
            throw std::runtime_error("动态音量需要同时设置时间和有效音频索引");
        if (dump_config) { std::cout << esports::serialize_live_config(options) << '\n'; return 0; }
        if (check_config) { std::cout << "配置校验通过（未打开媒体或输出）\n"; return 0; }
        return esports::run_live(options, stop_requested, log);
    } catch (const std::exception& e) {
        log.log(esports::LogLevel::Error, e.what());
        return 1;
    }
}
