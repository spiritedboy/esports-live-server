/*
 * Phase 2入口：单路解码、格式转换、重新编码。
 * 保留为学习与回归测试入口，完整多源节目请从live_main.cpp阅读。
 */
#include "manager/transcoder.h"
#include <cerrno>
#include <iostream>
#include <sys/stat.h>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void stop_handler(int) { stop_requested = 1; }

void usage() {
    std::cout << "用法：esports_transcode --input <本地文件|rtsp://...|rtmp://...> "
                 "--output <新建.flv|rtmp://...> [--width 640] [--height 360] [--no-realtime] [--timeout-ms 5000]\n"
                 "默认按 DTS 实时节奏发送；禁止覆盖已有文件；H.264/H.265/AAC 解码后编码为 H.264/AAC。\n";
}
}

int main(int argc, char** argv) {
    esports::Logger log(std::cerr);
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        esports::TranscodeOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string argument(argv[i]);
            if (argument == "--help") { usage(); return 0; }
            if (argument == "--no-realtime") { options.realtime = false; continue; }
            if (argument != "--input" && argument != "--output" && argument != "--timeout-ms" && argument != "--width" && argument != "--height")
                throw std::runtime_error("未知选项：" + argument);
            if (++i == argc) throw std::runtime_error("选项缺少参数：" + argument);
            if (argument == "--input") options.input = argv[i];
            else if (argument == "--output") options.output = argv[i];
            else if (argument == "--width" || argument == "--height") {
                const std::string value(argv[i]);
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("尺寸必须是整数");
                const unsigned long size = std::stoul(value);
                if (size < 16 || size > 3840 || size % 2) throw std::runtime_error("尺寸必须是 16–3840 的偶数");
                if (argument == "--width") options.width = size;
                else options.height = size;
            } else {
                const std::string value(argv[i]);
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("超时必须是整数毫秒");
                const unsigned long parsed = std::stoul(value);
                if (parsed < 100 || parsed > 300000)
                    throw std::runtime_error("超时范围为 100–300000 毫秒");
                options.timeout_ms = static_cast<int>(parsed);
            }
        }
        if (options.input.empty() || options.output.empty()) {
            usage();
            throw std::runtime_error("必须提供输入和输出");
        }
        if (!esports::is_network_input(options.input)) {
            if (options.input.find("://") != std::string::npos || options.input.find(':') != std::string::npos)
                throw std::runtime_error("不支持的输入协议");
            struct stat info = {};
            if (stat(options.input.c_str(), &info) < 0)
                esports::ff_check(AVERROR(errno), "检查输入文件");
            if (!S_ISREG(info.st_mode)) throw std::runtime_error("本地输入必须是普通文件");
        }
        if (!esports::starts_with(options.output, "rtmp://") &&
            options.output.find(':') != std::string::npos)
            throw std::runtime_error("输出仅支持本地 FLV 文件或 rtmp://");
        return esports::run_transcode(options, stop_requested, log);
    } catch (const std::exception& error) {
        log.log(esports::LogLevel::Error, error.what());
        return 1;
    }
}
