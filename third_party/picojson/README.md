# PicoJSON 来源

- 上游：https://github.com/kazuho/picojson
- 固定版本：v1.3.0（本项目不在构建时联网下载）。
- 文件：picojson.h（26102字节）、LICENSE，原样保留。
- 许可证：BSD-2-Clause，见同目录LICENSE。
- picojson.h SHA-256：5ddf7276d04926da7be243e7af49258e78cc27278ee9097ba45b942c7a6b5f9d。

仅esports_config私有包含此头文件，无系统库或运行期安装要求。
项目适配层限制为扁平配置对象、最多4项的标量数组、有限数字、有效UTF-8，
使用上游事件接口拒绝重复键，并额外检查JSON数字词法和尾随内容。
未使用库的全局last_error/流提取接口。更新此依赖须重新执行config_integration。
