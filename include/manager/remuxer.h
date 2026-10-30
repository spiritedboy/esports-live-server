#ifndef ESPORTS_MANAGER_REMUXER_H
#define ESPORTS_MANAGER_REMUXER_H
#include "common/ffmpeg.h"
#include "common/logger.h"

namespace esports {
struct RemuxOptions {
    std::string input;
    std::string output;
    bool realtime = true;
    int timeout_ms = 5000;
};
// 0：正常 EOF；130：用户停止；其他失败通过异常报告。
int run_remux(const RemuxOptions& options, const volatile std::sig_atomic_t& stop, Logger& log);
}
#endif
