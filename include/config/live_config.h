/*
 * 初学者导读
 * 配置有两步校验：此处检查 JSON 结构和字段类型，live_main 检查范围及字段之间的关系。
 * load_live_config 返回类似命令行的字符串列表，并不创建输入源或访问 RTMP 服务。
 * 这样 JSON 与命令行最终经过同一套参数解析，避免两条入口允许不同的非法值。
 * serialize_live_config 将最终 LiveOptions 转回 JSON，便于核对默认值和命令行覆盖结果。
 * 序列化结果包含实际 URL；查看输出时要留意 URL 中可能带有摄像头账号信息。
 */
/*
 * JSON与LiveOptions之间的启动适配接口，完整字段及例子见docs/configuration.md。
 */
#ifndef ESPORTS_CONFIG_LIVE_CONFIG_H
#define ESPORTS_CONFIG_LIVE_CONFIG_H
#include "manager/live_pipeline.h"
namespace esports {
// JSON只转换成既有选项；语义范围与跨字段关系由命令行的同一入口校验。
// path 必须指向普通配置文件；读取/语法/类型失败抛异常，成功返回待合并的参数序列。
std::vector<std::string> load_live_config(const std::string& path);
// 输入应是已校验、已补齐默认值的选项；返回 JSON 字符串，不自行写文件。
std::string serialize_live_config(const LiveOptions& options);
}
#endif
