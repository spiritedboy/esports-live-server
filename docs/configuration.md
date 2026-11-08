# esports_live 配置说明

自2026-10-30起，config/server.json是实际可加载的配置文件。
仅esports_live支持--config；独立esports_replay使用自己的命令行参数。
不指定--config时保留原来的纯命令行行为，不会自动搜索或加载文件。

## 启动与检查

从项目根目录执行。新环境先生成示例素材（脚本拒绝覆盖，已有素材无需重跑）：

```bash
bash scripts/generate-mix-samples.sh
bash scripts/generate-audio-samples.sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
./build/debug/esports_live --config config/server.json --check-config
./build/debug/esports_live --config config/server.json
```

仓库示例是一个3秒本地节目：双画面、两路音频、FLV输出、MP4分段和两个CSV。
输出路径在build/samples/config-*，必须尚不存在；再次运行需修改输出、录制前缀及CSV路径。
启动示例不需要RTMP服务端，不会部署任何服务。

```bash
# 不打开媒体、不连接网络、不创建输出；仅检验配置类型、范围和字段关系。
./build/debug/esports_live --config config/server.json --check-config

# 显示默认值和所有覆盖应用后的有效配置，随后退出。
./build/debug/esports_live --config config/server.json --width 1280 --height 720 --dump-config

# 命令行可以出现在--config之前，优先级不变。
./build/debug/esports_live --fps 30 --config config/server.json --no-realtime --check-config
```

check/dump不会确认输入文件存在、编解码器可用、输出目录可写或网络端点可达。
实际启动时仍执行媒体层检查。dump优先于check，输出标准JSON，可作为下一次配置输入。
dump会包含配置中的URL等原值，请自行保护输出中的推流凭据；普通解析错误不回显JSON原始片段。

## 合并与路径规则

顺序为：C++默认值 → 指定JSON → 命令行覆盖 → 数值范围及跨字段校验 → 创建流水线。
JSON语法、类型和未知字段无论是否被CLI覆盖都必须有效；被CLI覆盖的标量/逐路数字
不再参与最终范围校验。例如JSON fps=0被--fps 25覆盖后可以通过。
仅允许一个--config，重复指定报错；不会合并多个文件，也不支持include或环境变量展开。

- 标量：同名命令行覆盖JSON，例如--width、--output、--record-prefix。
- inputs/audio_inputs：只要命令行出现--input/--audio-input，就用该命令行列表完整替换对应JSON列表，不追加。
- gains/muted/偏移：按索引覆盖，不清空其他索引。--gain 0:0.8只改变第0路。
- --realtime和--no-realtime可双向覆盖realtime；--mute与--unmute可双向覆盖单路静音。
- CLI替换输入列表后，JSON剩余逐路参数仍按新列表索引解释；越界会报错，不会自动重新匹配源。
- gains/muted/偏移数组可以少于源数量，未填写位置使用默认值；不能超过最终对应源数量。
- audio_inputs缺省或空数组时继承最终inputs；gain/mute/audio_offsets按最终音频列表编号。
- 可选定时布局/增益变更通过省略相关字段禁用；启用时必须成对提供时间和目标。

**所有本地相对路径均相对于进程当前工作目录，不是JSON所在目录。**
包括输入、输出、录制前缀、CSV路径；这与已有CLI保持一致。异地启动时请使用绝对路径，
或先切换到项目目录。JSON为UTF-8，不支持注释、尾随逗号、重复键、null和嵌套对象。
文件须为普通文件，最大1MiB；数组最多4项，数字词法长度最多128字符。
未知字段报错，避免拼错配置却悄悄使用默认值。

## 字段、默认值和命令行映射

JSON顶层为扁平对象，字段使用snake_case。所有整数参数允许数学上为整数的JSON数字，
例如25.0，但不接受字符串"25"、布尔值true或有小数部分的25.5。
以下默认值指代码默认值；仓库示例会显式设置节目素材、3秒时长、录制和诊断文件。

| JSON字段 | 类型/默认值 | CLI与约束 | 示例与含义 |
|---|---|---|---|
| inputs | 字符串数组，无默认 | --input，可重复；最终2或4路，本地/RTSP/RTMP | `["build/samples/mix/red.mp4", "build/samples/mix/lime.mp4"]`：两路视频；网络地址见下方网络示例 |
| audio_inputs | 字符串数组，默认继承inputs | --audio-input，可重复；最多4路 | `[]`：继承视频源列表中的音轨；也可填独立音频文件/URL |
| output | 字符串，无默认 | --output；必填，本地FLV或rtmp:// | `"rtmp://192.0.2.10/live/esports"`：向已有服务端推流；本地输出可用 "build/samples/demo.flv" |
| layout | 字符串，split | --layout；split/pip/grid，grid需要4路 | `"split"`：左右双画面；pip为画中画，grid为四宫格 |
| width | 整数，640 | --width；16–3840且为4的倍数 | `1280`：输出宽1280像素 |
| height | 整数，360 | --height；16–2160且为4的倍数 | `720`：输出高720像素 |
| fps | 整数，25 | --fps；1–60 | `30`：输出每秒30帧 |
| duration_ms | 整数，0 | --duration-ms；0–86400000，0运行至EOF或停止 | `3000`：运行3秒媒体时间；持续直播用0 |
| realtime | 布尔，true | --realtime/--no-realtime；RTMP必须true | `true`：按媒体时间实时调度；本地离线处理可用false |
| stale_ms | 整数，500 | --stale-ms；0–60000 | `500`：最近视频帧过期阈值500ms，超期以黑帧替代 |
| timeout_ms | 整数，5000 | --timeout-ms；100–300000，输入超时 | `5000`：输入I/O超时阈值5秒 |
| gains | 数字数组，逐路1 | --gain 索引:值；0–4 | `[1.0, 0.5]`：第0路原音量，第1路线性幅度减半，约−6dB |
| muted | 布尔数组，逐路false | --mute 索引/--unmute 索引 | `[false, true]`：第0路不静音，第1路静音 |
| audio_offsets_ms | 整数数组，逐路0 | --audio-offset-ms 索引:值；−60000–60000 | `[0, 100]`：第1路音频时间轴后移100ms |
| video_offsets_ms | 整数数组，逐路0 | --video-offset-ms 索引:值；−60000–60000 | `[0, -100]`：第1路视频时间轴前移100ms；不是负延迟传输 |
| switch_at_ms | 可选整数，未启用 | --switch-at-ms；0–86400000，须搭配switch_layout | `1000`：节目媒体时间1秒时切换布局，与switch_layout成对 |
| switch_layout | 可选字符串，未启用 | --switch-layout；同layout，须搭配switch_at_ms | `"pip"`：切换为画中画，与switch_at_ms成对 |
| change_at_ms | 可选整数，未启用 | --change-at-ms；0–86400000，须搭配change_gain | `2000`：节目媒体时间2秒时修改增益，与change_gain成对 |
| change_gain | 可选数组[索引,增益] | --change-gain 索引:值；索引有效、增益0–4 | `[1, 0.8]`：把第1路音频增益改为0.8；静音状态需另行解除 |
| video_bitrate | 整数，1200000 | --video-bitrate；64000–20000000，bit/s | `2500000`：视频目标码率2.5Mbit/s，不含音频和协议开销 |
| gop_ms | 整数，1000 | --gop-ms；100–5000 | `1000`：目标关键帧间隔1秒，按帧率换算 |
| vbv_ms | 整数，500 | --vbv-ms；50–2000 | `500`：编码码率控制缓冲时长500ms，不等于端到端延迟 |
| video_queue_frames | 整数，4 | --video-queue-frames；1–64 | `4`：每路视频最多缓存4帧 |
| audio_queue_blocks | 整数，16 | --audio-queue-blocks；1–128 | `16`：每路音频最多缓存16个块 |
| output_timeout_ms | 整数，3000 | --output-timeout-ms；100–300000 | `3000`：输出I/O超时阈值3秒 |
| reconnect_attempts | 整数，5 | --reconnect-attempts；0–100 | `5`：网络输出恢复重试预算5次；0不重试，不控制输入重连 |
| retry_backoff_ms | 整数，250 | --retry-backoff-ms；10–4000 | `250`：重试初始退避250ms，指数增长且最多4000ms |
| output_latency_ms | 整数，500 | --output-latency-ms；50–10000 | `500`：输出队列媒体时间跨度上限500ms，不是播放器总延迟保证 |
| packet_queue_count | 整数，256 | --packet-queue-count；4–4096 | `256`：输出队列最多256个编码包 |
| packet_queue_bytes | 整数，8388608 | --packet-queue-bytes；4096–67108864 | `8388608`：输出队列编码数据最多8MiB，与包数/时间跨度共同约束 |
| record_prefix | 字符串，空（不录制） | --record-prefix；本地路径前缀，禁止冒号/制表符/换行 | `"build/samples/demo-record"`：录制分段和索引的路径前缀；空字符串禁用录制 |
| record_format | 字符串，mp4 | --record-format；mp4/mkv | `"mp4"`：录制为MP4；也可用mkv |
| segment_ms | 整数，60000 | --segment-ms；100–86400000 | `60000`：目标每段60秒，在后续IDR处分段，不保证恰好60秒 |
| record_queue_bytes | 整数，16777216 | --record-queue-bytes；1024–67108864 | `16777216`：独立录制队列最多16MiB；溢出停用录制 |
| stats_file | 字符串，空（不写） | --stats-file；同步/输出CSV新文件 | `"build/samples/demo-sync.csv"`：同步与输出统计CSV；空字符串禁用 |
| metrics_file | 字符串，空（不写） | --metrics-file；性能CSV新文件 | `"build/samples/demo-metrics.csv"`：CPU/RSS/FPS/耗时CSV；空字符串禁用 |

输入音视频编号从0开始。JSON中的null不能用来禁用项目：诊断文件/录制前缀用空字符串，
定时变更省略整对字段。配置文件不支持运行中热更新；增益/布局的定时变化仍由既有媒体时间轴执行。

## 可直接加载的场景示例

| 文件 | 用途 |
|---|---|
| [server.json](../config/server.json) | 原有3秒本地节目，保持原配置 |
| [server.full-example.json](../config/server.full-example.json) | 全部36个字段，包含1秒切到画中画、2秒将第1路音频增益改为0.8 |
| [server.network-example.json](../config/server.network-example.json) | RTMP游戏画面 + RTSP摄像头输入，H.264/AAC经RTMP输出；其余字段使用默认值 |

完整示例中的音视频素材由开头两条脚本生成，输出路径单独设置。先在项目根目录校验，再运行：

```bash
./build/debug/esports_live --config config/server.full-example.json --check-config
./build/debug/esports_live --config config/server.full-example.json
```

JSON不支持注释，字段说明请查上表。表中的值展示各字段用法；需要一份参数关系一致的完整配置时，
直接使用完整示例。要取消定时切换，删除switch_at_ms/switch_layout整对；取消定时增益则删除
change_at_ms/change_gain整对。所有调度时间均为节目媒体时间，不是系统时钟。

### RTMP/RTSP输入与RTMP输出

网络示例使用文档专用占位地址192.0.2.x，必须替换成你已有的源和服务端地址、端口、流路径。
RTSP摄像头输入采用TCP；视频需要H.264/H.265，音频需要AAC/Opus，当前不支持G.711解码。
audio_inputs为[]会继承两路视频URL的音轨，不表示关闭音频；无可用音轨的源会降级静音。
也可以显式填写独立音频源。音视频目前分别打开同一URL，设备连接数和时间轴对齐需实测。

```bash
# 仅校验，不连接示例中的网络地址。
./build/debug/esports_live --config config/server.network-example.json --check-config
# 替换成实际地址后运行；Ctrl+C停止并收尾。
./build/debug/esports_live --config config/server.network-example.json
```

duration_ms为0不会循环播放本地文件；实时源也可能结束或失败。
reconnect_attempts等恢复参数仅针对网络输出，输入尚不支持自动重连。
本程序拉取输入流并向已有RTMP接收端推流，不充当OBS可直接推入的RTMP服务端。
此示例通过配置校验不代表真实RTMP/RTSP会话验收；成功接收、播放和断网恢复仍需实际端点验证。

### 四宫格与本地离线输出

在完整示例基础上用命令行替换视频列表为4路，取消实时节奏：

```bash
./build/debug/esports_live --config config/server.full-example.json \
  --input build/samples/mix/red.mp4 --input build/samples/mix/lime.mp4 \
  --input build/samples/mix/red.mp4 --input build/samples/mix/lime.mp4 \
  --layout grid --switch-layout grid --no-realtime \
  --output build/samples/grid-example.flv --record-prefix "" \
  --stats-file "" --metrics-file "" --check-config
```

去掉--check-config后实际运行；重复素材用于展示4路配置，不代表4个独立设备。
这里同时覆盖switch_layout，防止完整示例在1秒时切回画中画；独立音频仍为原来的两路。
本地输出文件必须尚不存在，输出父目录必须已存在。

## 实现与验证

加载层位于src/config/live_config.cpp，单独CMake目标esports_config。
PicoJSON v1.3.0的固定单头文件和BSD-2-Clause许可证保存在third_party/picojson，
构建不联网，不需要安装JSON系统包；来源和校验值见该目录README。
加载器生成既有选项供同一入口处理，不创建第二套媒体参数默认值或另一条处理链路。

```bash
ctest --test-dir build/debug -R config_integration --output-on-failure
```

用例覆盖优先级、列表替换、逐路覆盖、反向布尔覆盖、导出往返、字段/类型/范围错误、
重复键/畸形数字/UTF-8/NUL/大文件/FIFO拒绝，以及JSON真实启动、中文路径、录制解码和文件保护。
