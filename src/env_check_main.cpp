/*
 * 环境检测入口：比较编译头文件与运行库版本，查询编码器/封装器/协议是否注册。
 * 这里不创建媒体流水线；能力存在并不等于真实推流已通过验收。
 */
#include "common/logger.h"

#include <iostream>
#include <sstream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {
std::string version_string(unsigned version) {
    std::ostringstream out;
    out << AV_VERSION_MAJOR(version) << '.' << AV_VERSION_MINOR(version)
        << '.' << AV_VERSION_MICRO(version);
    return out.str();
}

bool check_library(esports::Logger& log, const char* name,
                   unsigned headers, unsigned runtime) {
    const bool compatible = AV_VERSION_MAJOR(headers) == AV_VERSION_MAJOR(runtime);
    log.log(compatible ? esports::LogLevel::Info : esports::LogLevel::Error,
            std::string(name) + "：头文件=" + version_string(headers) +
            "，运行库=" + version_string(runtime) +
            (compatible ? "，主版本一致" : "，主版本不一致"));
    return compatible;
}

bool has_rtmp_output() {
    void* opaque = nullptr;
    const char* name = nullptr;
    while ((name = avio_enum_protocols(&opaque, 1)) != nullptr) {
        if (std::string(name) == "rtmp") return true;
    }
    return false;
}
}

int main() {
    esports::Logger log(std::cout);
    log.log(esports::LogLevel::Info,
            std::string("FFmpeg 版本：") + av_version_info());
    bool compatible = true;
    compatible &= check_library(log, "libavformat", LIBAVFORMAT_VERSION_INT, avformat_version());
    compatible &= check_library(log, "libavcodec", LIBAVCODEC_VERSION_INT, avcodec_version());
    compatible &= check_library(log, "libavutil", LIBAVUTIL_VERSION_INT, avutil_version());
    compatible &= check_library(log, "libavfilter", LIBAVFILTER_VERSION_INT, avfilter_version());
    compatible &= check_library(log, "libswresample", LIBSWRESAMPLE_VERSION_INT, swresample_version());
    compatible &= check_library(log, "libswscale", LIBSWSCALE_VERSION_INT, swscale_version());

    // 查询返回的描述符由 FFmpeg 管理，不得释放；查询不代表实际编码成功。
    const bool h264 = avcodec_find_encoder_by_name("libx264") != nullptr;
    const bool aac = avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr;
    const bool flv = av_guess_format("flv", nullptr, nullptr) != nullptr;
    const bool rtmp = has_rtmp_output();
    log.log(esports::LogLevel::Info,
            std::string("能力注册：libx264=") + (h264 ? "有" : "无") +
            "，AAC=" + (aac ? "有" : "无") + "，FLV=" + (flv ? "有" : "无") +
            "，RTMP输出协议=" + (rtmp ? "有" : "无"));
    if (!(h264 && aac && flv && rtmp)) {
        log.log(esports::LogLevel::Warning, "部分后续阶段能力未注册，需在相应阶段处理。");
    }
    log.log(compatible ? esports::LogLevel::Info : esports::LogLevel::Error,
            compatible ? "环境检测通过（未进行媒体或网络测试）" : "环境检测失败");
    return compatible ? 0 : 1;
}
