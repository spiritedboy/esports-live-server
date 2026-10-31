/*
 * Phase 3入口：配置2/4路视频合成和定时布局切换，仅输出视频。
 * 音视频完整节目由esports_live负责；这里保留阶段实现以便逐步学习。
 */
#include "manager/compositor.h"
#include <iostream>
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
    esports::Logger log(std::cerr);
    try {
        esports::CompositeOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string key(argv[i]);
            if (key == "--help") {
                std::cout << "用法：esports_composite --input A --input B [--input C --input D] "
                    "--output 新文件.flv|rtmp://... [--layout split|grid|pip] "
                    "[--width 640 --height 360 --fps 25] [--duration-ms 0] "
                    "[--stale-ms 500] [--timeout-ms 5000] [--no-realtime] "
                    "[--switch-at-ms 1000 --switch-layout pip]\n";
                return 0;
            }
            if (key == "--no-realtime") { options.realtime = false; continue; }
            if (++i == argc) throw std::runtime_error("参数缺少值");
            const std::string value(argv[i]);
            if (key == "--input") options.inputs.push_back(value);
            else if (key == "--output") options.output = value;
            else if (key == "--layout") options.layout = value;
            else if (key == "--switch-layout") options.switch_layout = value;
            else if (key == "--switch-at-ms") options.switch_at_ms = integer(value, 0, 86400000);
            else if (key == "--duration-ms") options.duration_ms = integer(value, 0, 86400000);
            else if (key == "--stale-ms") options.stale_ms = integer(value, 0, 60000);
            else if (key == "--timeout-ms") options.timeout_ms = integer(value, 100, 300000);
            else if (key == "--width") options.width = integer(value, 16, 3840);
            else if (key == "--height") options.height = integer(value, 16, 2160);
            else if (key == "--fps") options.fps = integer(value, 1, 60);
            else throw std::runtime_error("未知选项：" + key);
        }
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
        return esports::run_composite(options, stop_requested, log);
    } catch (const std::exception& e) {
        log.log(esports::LogLevel::Error, e.what());
        return 1;
    }
}
