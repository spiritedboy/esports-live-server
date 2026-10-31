/*
 * Phase 2在输入/输出配置上增加编码尺寸和名义帧率；保持单路媒体时间关系。
 */
#ifndef ESPORTS_MANAGER_TRANSCODER_H
#define ESPORTS_MANAGER_TRANSCODER_H
#include "manager/remuxer.h"
namespace esports {
struct TranscodeOptions : RemuxOptions {
    int width = 640;
    int height = 360;
    int fps = 25; // 编码器名义帧率；单路转码保持原始 PTS，不强制 CFR。
};
int run_transcode(const TranscodeOptions&, const volatile std::sig_atomic_t&, Logger&);
}
#endif
