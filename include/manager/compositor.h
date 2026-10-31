/*
 * Phase 3视频节目配置；时间参数为毫秒，switch_at_ms=-1表示不安排切换。
 */
#ifndef ESPORTS_MANAGER_COMPOSITOR_H
#define ESPORTS_MANAGER_COMPOSITOR_H
#include "manager/transcoder.h"
#include <vector>
namespace esports {
struct CompositeOptions : TranscodeOptions {
    std::vector<std::string> inputs;
    std::string layout = "split";
    std::string switch_layout;
    int switch_at_ms = -1;
    int duration_ms = 0; // 0：运行到全部文件 EOF 或用户停止。
    int stale_ms = 500;
};
int run_composite(const CompositeOptions&, const volatile std::sig_atomic_t&, Logger&);
}
#endif
