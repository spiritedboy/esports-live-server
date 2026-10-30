#ifndef ESPORTS_CONFIG_LIVE_CONFIG_H
#define ESPORTS_CONFIG_LIVE_CONFIG_H
#include "manager/live_pipeline.h"
namespace esports {
// JSON只转换成既有选项；语义范围与跨字段关系由命令行的同一入口校验。
std::vector<std::string> load_live_config(const std::string& path);
std::string serialize_live_config(const LiveOptions& options);
}
#endif
