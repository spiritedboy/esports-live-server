# Esports Live Server

基于 Ubuntu/WSL2、C++11、CMake 和 FFmpeg C API 的音视频学习与研发项目。
目标：输入 → 解封装 → 解码 → 视频合成/音频混音 → 编码 → RTMP/录制。
当前实现至 **Phase 7**，监控、短时压力测试和性能采样已加入；真实 RTMP、24小时与长期稳定性验收仍未完成。不部署 SRS，不实现 WebRTC。

## 当前功能与验收边界

| 程序 | 用途 | 当前验收 |
|---|---|---|
| ffmpeg_env_check | 六库版本和注册能力检测 | 本机通过 |
| esports_remux | H.264/AAC 不转码，输出 FLV/RTMP | 本地链路及网络失败路径通过 |
| esports_transcode | H.264/H.265/AAC 解码，H.264/AAC 重编码 | 本地视频/音频转换及排空通过 |
| esports_composite | 2/4 路视频、双画面/四宫格/画中画 | 像素、调度、故障和有界队列测试通过 |
| esports_live | 视频合成、混音、同步、直播并行分段录制 | 本地与故障路径通过 |
| esports_replay | MP4/MKV 关键帧查询、片段提取、实时回放 | 本地定位/解码/节奏通过 |

Phase 2 先完成并通过 Debug/Release/ASan 检查，再实施 Phase 3。
旧 esports_composite 保持仅视频行为；新增 esports_live 提供 Phase 4 音视频混合输出。
成功 RTSP/RTMP 会话、实际推流播放、人工视听及长期稳定性尚未验收。
GDB 已验证，VSCode 图形界面的 F5 断点仍待人工验收。
Git 没有提交。config/server.json 已接入 esports_live，支持JSON配置及命令行覆盖，详见[配置说明](docs/configuration.md)。

## 环境

实测：Ubuntu 24.04.4 / WSL2，GCC/G++ 13.3.0，CMake 3.28.3，
pkg-config 1.8.1，FFmpeg 6.1.1，GDB 15.1，Python 3.12.3。
通过 PkgConfig::FFMPEG 链接 avformat、avcodec、avutil、avfilter、swresample、swscale，
通过 Threads::Threads 链接线程依赖。强制 -std=c++11，禁用 GNU 扩展。

新 Ubuntu 环境可安装（本次没有改变系统依赖）：

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config gdb git python3 ffmpeg \
  libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev \
  libswresample-dev libswscale-dev
```

编码需要 libx264 和原生 AAC 编码器；H.265/Opus 测试素材生成另需 libx265/libopus。
Python、ffmpeg、ffprobe 仅供集成测试及素材生成，核心程序不调用 FFmpeg CLI。

## 编译与测试

在项目根目录执行：

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
```

启用 -Wall/-Wextra/-Wpedantic；Debug 为 -g，Release 为 -O3/-DNDEBUG。
所有构建文件由 CMake 生成，没有手写 Makefile。
只构建程序可添加 -DBUILD_TESTING=OFF，跳过测试工具依赖。

CTest 包含环境检测、并发日志、Remux、转码、FrameQueue、多路合成、
audio_sync、live_integration、packet_queue、output_integration、recording_integration、metrics_integration、video_reference、process_runner、recording_write_failure 和 config_integration 共十六项。Phase 4 可用 -R 'audio_sync|live_integration' 单独测试。
测试使用真实生成的媒体，素材在各构建目录创建和清理；测试判断不依赖 assert。
可用 -R transcode 或 -R 'frame_queue|composite' 单独测试对应阶段。

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -no-pie"
cmake --build build/asan --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan --output-on-failure
```

短测试未报告内存错误，不代表全部协议或长期运行无问题；系统 FFmpeg 未重新插桩。

## Phase 0 / 1：环境与转封装

```bash
./build/debug/ffmpeg_env_check
./scripts/generate-sample.sh
./build/debug/esports_remux --input build/samples/input.mp4 --output build/samples/remux-demo.flv
```

样片为 3 秒、320×180/25fps、含 B 帧 H.264 + 48kHz AAC。
默认按 DTS 实时发送，离线快速处理加 --no-realtime。
支持本地文件、rtsp://（TCP）、rtmp:// 输入；输出本地 FLV 或 rtmp://。
最多一路视频和一路音频，可只有其中一路；拒绝不兼容编码或缺失/回退时间戳。

## Phase 2：单路转码

```bash
./build/debug/esports_transcode --input build/samples/input.mp4 \
  --output build/samples/transcode-demo.flv --width 640 --height 360 --no-realtime
```

H.264/H.265 视频和 AAC 音频解码后，输出 H.264/YUV420P 与 AAC/48kHz/双声道。
尺寸默认为 640×360，必须是 16–3840 的偶数。视频保持源帧 PTS，不强制 CFR。
支持单音轨、单视频；音频保留起点偏移，参数中途变化或明显间断会报错。
默认实时发送。包含解码器、重采样器、音频 FIFO 和编码器的 EOF 排空。

## Phase 3：多路视频合成

生成四路不同尺寸、不同帧率的素材，并合成为四宫格：

```bash
./scripts/generate-mix-samples.sh
./build/debug/esports_composite \
  --input build/samples/mix/red.mp4 --input build/samples/mix/lime.mp4 \
  --input build/samples/mix/blue.mp4 --input build/samples/mix/yellow.mp4 \
  --output build/samples/grid-demo.flv --layout grid --width 640 --height 360 \
  --fps 25 --no-realtime
```

双画面，1 秒时切换为右下画中画：

```bash
./build/debug/esports_composite \
  --input build/samples/mix/red.mp4 --input build/samples/mix/lime.mp4 \
  --output build/samples/switch-demo.flv --layout split \
  --switch-at-ms 1000 --switch-layout pip
```

- 输入必须为 2 或 4 路，grid 必须为 4 路；split/pip 使用前两路画面。
- 每路允许一个 H.264/H.265 视频轨，音轨被忽略，不混音也不透传音轨。
- 宽高必须为 4 的倍数，宽 16–3840、高 16–2160，输出 fps 为 1–60。
- 默认实时调度；--no-realtime 用于离线验收，按时间戳读取未来帧后再输出。
- 每路以首个视频 PTS 为零点，视为同时开始。这不是多设备真实时钟对齐。
- 每路最多缓存 8 帧；输出时选取不晚于当前时刻的最新帧，高帧率舍弃中间帧，低帧率重复最近帧。
- 停帧超过 --stale-ms（默认 500ms）使用黑帧；低于 2fps 的源可能需要增大该值。
- 单路源失败记录警告并降级，其他源继续。全部源初始无帧则失败退出。
- --duration-ms 指定节目时长，0 表示到全部文件耗尽。网络源可运行到 Ctrl+C；无重连。
- 切换布局在指定时刻之后的第一个输出帧生效，编码器和输出时间轴保持连续。
- 四路输入切换到 split/pip 时仍读取全部源，节目结束判断仍考虑全部源。

## Phase 4：混音与音画同步

先生成视频素材（已生成则跳过）和两路音频素材：

```bash
./scripts/generate-mix-samples.sh
./scripts/generate-audio-samples.sh
./build/debug/esports_live \
  --input build/samples/mix/red.mp4 --input build/samples/mix/lime.mp4 \
  --audio-input build/samples/audio/game.m4a --audio-input build/samples/audio/voice.opus \
  --output build/samples/av-demo.flv --duration-ms 3000 \
  --gain 0:0.7 --gain 1:0.5 --stats-file build/samples/av-demo.csv --no-realtime
```

输出 H.264 视频和 AAC/48kHz/双声道音频。
支持 AAC、Opus 音频输入，内部统一为 FLTP 采样格式。
不指定 --audio-input 时，按视频输入列表分别读取对应音轨；
指定后只混合列出的 1–4 路音频。没有音轨或失败的源填静音并记录日志。

| 参数 | 含义 |
|---|---|
| --gain 0:0.7 | 第 0 路音频线性增益 0.7，范围 0–4 |
| --mute 1 | 静音第 1 路；仍消费其队列以维持时间位置 |
| --change-at-ms 1000 --change-gain 1:0 | 在输出约 1 秒处将第 1 路增益渐变为零 |
| --audio-offset-ms 0:-250 | 第 0 路音频提前 250ms；负时间部分被裁掉 |
| --video-offset-ms 0:100 | 第 0 路视频延后 100ms，之前为黑帧 |
| --stats-file 新文件.csv | 输出同步采样记录，禁止覆盖已有文件 |

索引从 0 开始，音频和视频偏移使用各自列表的索引；偏移范围 ±60000ms。
增益变更按音频块边界开始，约 5ms 平滑过渡；静音状态优先于增益值。
混音后统一峰值限制为 0.95（编码前 PCM），快速抑制、约 100ms 恢复；
它会压缩过大的信号，不能代替正确设置输入电平。AAC 重建峰值不保证同一上限。

源时钟使用共同容器起点，保留同源音视频相对偏移；不同文件视为同起点，
真实设备偏移需手动配置。容器缺少时间信息时会回退并记录，不能承诺保留未知偏移。
视频跟随 48kHz 采样时间轴调度，音频以连续采样块输出。
微小音频相位误差通过 swr_set_compensation 调整；大于 100ms 的间断重建重采样器，
留下的时间缺口用静音填补；回退/重复/超过 2 秒的异常跳变重建源时间轴。
异常边界尚未加入交叉淡化，不保证任意损坏输入的听感。

每路音频队列最多 32 块，每块最多 8192 采样；每路视频队列最多 8 帧。
默认正常 EOF 结束；--duration-ms 可继续输出到指定时长，耗尽源使用黑帧/静音。
默认实时发送，离线验证可加 --no-realtime。
所有输入都失败则报错；部分输入失败时允许降级输出，必须同时检查日志。

CSV 字段：
media_us 为调度时刻；video_end_us/audio_end_us 为已送编码的媒体末端；
av_delta_us 是二者差值；wall_lag_us 是相对单调时钟的进度差。
**av_delta_us 不是测量到的内容口型误差**，25fps 下约 40ms 的交替调度差属于块粒度。
离线加速时 wall_lag_us 为负是正常现象。源音频相位误差和补偿次数另在日志中报告。

实测包括 30 秒媒体输出，以及 ±1000ppm、600 秒媒体时间的加速漂移模拟；
这不等于实际运行 10 分钟，更不等于真实设备/24 小时稳定性验收。

## 输出、网络与停止

所有输出始终为 FLV 容器，与文件扩展名无关。脚本和程序都禁止覆盖已有文件；
重复运行请换一个输出路径，输出目录必须存在。失败可能保留部分文件用于排查。
退出码 0 表示该命令正常结束，1 表示失败，130 表示用户停止。
Remux/转码的网络 EOF 作为连接结束或断流返回失败，不自动重连。
合成中单路失败会降级，完整程度需同时查看日志，不能只看退出码。

需要已有服务端时，将上述输出换成 rtmp://HOST/live/stream（HOST 为占位）。
程序有实际 RTMP 输出实现，但没有可用接收端，因此尚未通过真实网络播放验收。
--timeout-ms 默认 5000，范围 100–300000，控制可中断 FFmpeg I/O。
回调依赖底层协议检查，不能保证 DNS 或内核磁盘阻塞的严格墙钟上界。
Ctrl+C/SIGTERM 触发有界清理；工作线程通过原子停止标志和队列关闭退出，全部 join。

独立验证输出：

```bash
ffprobe -v error -show_streams build/samples/grid-demo.flv
ffmpeg -v error -xerror -i build/samples/grid-demo.flv -f null -
# 具备图形/声音设备时再人工播放：
ffplay build/samples/grid-demo.flv
```

## VSCode / GDB

在 VSCode WSL 窗口打开项目，安装 WSL 端 Microsoft C/C++ 扩展。
Ctrl+Shift+B 构建 Debug，选择相应 Phase 调试配置，设置断点后按 F5。
Phase 2/3/4 先执行相应样片生成脚本，调试输出路径必须不存在。
compile_commands.json 由 CMake 生成并用于 IntelliSense。

## 目录与模块

```text
include/common、src/common       日志、FFmpeg RAII 与 I/O 中断
include/input、src/input         StreamInput、VideoSource、AudioSource 工作线程
include/output、src/output       FLV/RTMP、独立输出线程、重连及原子文件保护
include/codec、src/codec         解码器、编码器、视频/音频格式转换
include/queue/                  有界视频帧/音频块/编码包队列
include/mixer、src/mixer         libavfilter 视频合成、PCM 混音
include/manager、src/manager     Remux、转码、合成、音视频同步流水线
include/recording、src/recording 独立录制线程、分段与索引
include/monitor、src/monitor     编解码计时、CPU/RSS、输入与输出监控
include/sync、src/sync           源时钟修复、重采样漂移补偿
src/*_main.cpp                  命令行入口
scripts/                        素材生成（不执行核心处理）
tests/                          十六项 CTest 及媒体校验
docs/                           计划、学习笔记、架构、真实测试报告
build/                          生成目录，不入 Git
```

## 已知限制

已实现基于媒体 PTS 的音频漂移补偿和软件时间轴对齐；没有 NTP/硬件时钟同步、
自动测定设备间真实偏移；Phase 5 已加入输出重连，输入自动重连尚未实现。
当前音视频独立解复用，同一 URL 作为视频和音频源时会打开两次；
本地文件起点可复现，实时网络会话可能有不同起点，需要实测/配置偏移。
音频参数中途变化会使该音频源降级静音，尚不支持动态格式重配置。
目前同步日志和编码阻塞仍会影响调度；输出已移入独立线程，队列有界不等于端到端低延迟。
缩放直接铺满目标区域，可能改变宽高比；10-bit→8-bit 转换不代表 HDR 色调映射。
AAC 编码预卷和容器 priming/padding 差异未提供采样级无缝裁剪保证。
只做了短素材和故障测试，没有四路 1080p、24 小时稳定性或性能达标声明。

详见 [开发计划](docs/development-plan.md)、[学习笔记](docs/learning-notes.md)、
[架构说明](docs/architecture.md)、[测试报告](docs/performance-report.md)。

## Phase 5：独立输出、恢复与低延迟配置

本阶段扩展 esports_live。输入线程 → 视频合成/音频混音与编码 → 有界 PacketQueue →
AsyncOutput 工作线程 → FFmpeg FLV/RTMP。旧 Remux/转码/仅视频程序不自动启用重连。
本地 FLV 满队列采取背压，保留全部包；RTMP 积压时丢弃旧窗口，请求 IDR 并建立新会话。
不重发失败的 AVPacket。每个会话复制编码参数、重新写头，以首个 IDR DTS 为共同原点。

先按前文生成 mix 和 audio 素材，输出文件名必须未使用：

```bash
./build/debug/esports_live \
  --input build/samples/mix/red.mp4 --input build/samples/mix/lime.mp4 \
  --audio-input build/samples/audio/game.m4a --audio-input build/samples/audio/voice.opus \
  --output build/samples/phase5-local.flv --stats-file build/samples/phase5-local.csv \
  --duration-ms 3000 --width 320 --height 240 --fps 25 \
  --packet-queue-count 4 --video-queue-frames 4 --audio-queue-blocks 16
ffmpeg -v error -xerror -i build/samples/phase5-local.flv -f null -
ctest --test-dir build/debug -R 'packet_queue|output_integration' --output-on-failure
```

接入自己已有的 RTMP 地址时，将 --output 换成 rtmp://地址/应用/流名；
不要添加 --no-realtime。本项目不部署 RTMP 服务端。

| 参数 | 默认值 | 含义 |
|---|---:|---|
| --output-timeout-ms | 3000 | FFmpeg 打开/写入 I/O 超时 |
| --reconnect-attempts | 5 | 整次运行自动恢复预算，含连接错误及积压触发的重连；0 禁用 |
| --retry-backoff-ms | 250 | 指数退避初值，最高4000ms，可被取消 |
| --output-latency-ms | 500 | 网络包排队年龄、媒体跨度及调度落后预算 |
| --packet-queue-count | 256 | 排队编码包数量上限 |
| --packet-queue-bytes | 8388608 | 排队数据及附加数据字节上限 |
| --video-queue-frames | 4 | 每路视频输入队列容量 |
| --audio-queue-blocks | 16 | 每路音频输入队列容量 |
| --video-bitrate | 1200000 | H.264 目标码率，bit/s |
| --gop-ms | 1000 | 正常 GOP 时长；恢复可提前请求 IDR |
| --vbv-ms | 500 | 码率控制缓冲时长 |

CSV 新增已写包数、编码数据平均 kbps、丢包数、当前队列数量/字节、排队与写入峰值耗时、
恢复次数。末尾日志另含平均 fps、音视频丢包率和调度跳过数量。
丢包率 = 已丢包 / (已写包 + 已丢包)，不包括编码前跳过的帧/采样；
这些是应用层累计指标，不是接收端确认。kbps 不包含 FLV/TCP 开销。
平均 fps 按墙钟计算，--no-realtime 下是处理速度，不是输出媒体帧率。

队列上限不包含一个正在写出的包、编解码器、FFmpeg 和内核缓冲；
500ms 不是端到端延迟保证，单次 I/O 另受超时约束。
当前测试覆盖实际 RTMP 拒绝连接/握手超时，以及注入传输故障后的真实 FLV 恢复；
未完成真实 RTMP 成功推流、服务端重启恢复、播放器端到端延迟及长期 RSS 验收。


## Phase 6：赛事录制、片段提取与回放

esports_live 可增加独立录制支路，沿用已经编码的 H.264/AAC，不重复编码。
录制队列满或录制错误时记录 ERROR 并停用录制，直播继续；程序整体退出0不等于录制成功，
需检查“录制完成”日志和索引。索引只登记已成功写尾的段，不登记正在写入的段。

```bash
# 先用前文脚本生成 mix/audio 素材；所有输出路径必须尚不存在。
./build/debug/esports_live \
  --input build/samples/mix/red.mp4 --input build/samples/mix/lime.mp4 \
  --audio-input build/samples/audio/game.m4a --audio-input build/samples/audio/voice.opus \
  --output build/samples/phase6-live.flv \
  --record-prefix build/samples/phase6-record --record-format mp4 --segment-ms 1000 \
  --duration-ms 3000 --width 320 --height 240 --fps 25

# 查看节目时间到文件的映射，以及某段的关键帧位置。
cat build/samples/phase6-record.index.tsv
./build/debug/esports_replay --input build/samples/phase6-record-000001.mp4 --list-keyframes

# 按节目索引定位；1.2秒请求向前定位到所在段的1.0秒IDR。
./build/debug/esports_replay --index build/samples/phase6-record.index.tsv \
  --start-ms 1200 --duration-ms 500 --output build/samples/phase6-highlight.mkv

# 按DTS实时回放；也可把output换成已有RTMP接收地址。
./build/debug/esports_replay --input build/samples/phase6-highlight.mkv \
  --mode replay --output build/samples/phase6-replayed.flv
ffmpeg -v error -xerror -i build/samples/phase6-replayed.flv -f null -
ctest --test-dir build/debug -R recording_integration --output-on-failure
```

| 选项 | 含义 |
|---|---|
| --record-prefix | 本地录制前缀，省略则不录制；父目录须存在 |
| --record-format mp4\|mkv | 默认mp4，分段文件自动增加序号与扩展名 |
| --segment-ms | 默认60000；达到时长后的第一个IDR切段，实际长度受GOP约束 |
| --record-queue-bytes | 默认16MiB，上限64MiB；同时最多1024个排队包 |
| replay --input | 指定单个本地录像 |
| replay --index | 按节目时间选择索引中的已完成段，与input互斥 |
| replay --start-ms | 默认0；input模式为文件内时间，index模式为节目时间 |
| replay --duration-ms | 默认0到当前文件EOF；正值的终点是请求起点+时长 |
| replay --mode extract\|replay | 默认extract快速提取；replay按DTS节奏发送，RTMP必须使用此模式 |
| replay --list-keyframes | 扫描单个文件，输出关键帧时间和容器包位置，位置可能为−1 |

索引为TSV：segment、start_us、end_us、path，路径为绝对路径，时间区间为[start,end)。
移动录像后需要同步修正索引路径。索引不保证断电持久性，不做fsync或崩溃恢复。
正常MP4依赖写尾完成moov；进程被强杀或磁盘错误可能留下不能播放的当前段，
已完成段仍保留。文件和索引均排他创建，不覆盖既有文件/符号链接。

当前基础提取面向本项目无B帧H.264+AAC录像，B帧录像明确拒绝。
起点向前对齐关键帧，日志报告实际起点，因此提取内容可能长于请求时长；
终点按压缩包DTS截断，最后一个包的采样/帧时长可能略越界。
索引模式不自动拼接多段，跨段正时长请求明确拒绝；时长0仅到选中段末尾。
分段时AAC编码延迟可能使随后到达的旧音频包落在新原点之前，这部分预卷丢弃；
不是采样级无缝切段，段边界不承诺无音频缺口。回放不自动切入正在制作的节目。

输入读取和提取错误退出非零；回放实时发送支持SIGINT/SIGTERM，网络失败不自动重连。
真实RTMP边播边录与回放接收播放尚未验收。当前只验证本地并行输出，
未做磁盘写满/断电、持续慢磁盘、长期磁盘空间管理或人工听感验收。
录制线程隔离了正常运行中的磁盘等待，但进程收尾仍须join；任意内核磁盘阻塞无法保证限时退出。

新增模块：include/recording/recorder.h、src/recording/recorder.cpp、src/replay_main.cpp；
StreamOutput 复用排他文件AVIO并增加MP4/Matroska封装选择；tests/recording_integration.py验收整条链路。


## Phase 7：运行监控与可复现压力测试

esports_live增加 --metrics-file 新文件.csv，与 --stats-file 同时使用可分别观察进程资源和同步/输出。
每秒媒体进度采集一次，停止并join输入线程后记录最终计数。指标文件同样禁止覆盖；
诊断文件写失败会报告错误并结束主链路，不会静默丢弃统计。

| 指标 | 含义 |
|---|---|
| wall_us | 自监控启动的单调墙钟时间，含启动准备 |
| cpu_percent | 相邻窗口进程用户+内核CPU时间/墙钟时间×100，100%等于占满一个核 |
| rss_kib / peak_rss_kib | /proc/self/status当前RSS / getrusage高水位，读取时刻和内核统计口径不同 |
| threads | 采样时进程线程数，包括FFmpeg内部线程；不可读为−1 |
| input_video_fps / output_fps | 相邻窗口累计解码视频帧/成功写出视频包的变化率 |
| v0_frames…v3_frames / a0_frames…a3_frames | 各输入累计解码帧；音频帧是音频块，不是采样点 |
| decode_us / encode_us | FFmpeg send/receive API累计墙钟耗时，排除消费回调 |
| processing_us | 视频合成和音频混音调用累计耗时，不包括输入缩放/重采样 |
| video_queued / audio_queued | 所有输入队列当前总量，非同时刻严格快照 |
| audio_late / output_dropped | 过期音频块 / 输出丢包累计数 |
| skipped_video / skipped_audio | 网络追赶时跳过的调度视频帧/音频采样 |

编解码API可能把工作交给FFmpeg内部线程，因此累计API耗时不是所有工作线程CPU耗时。
输入FPS受文件读取、预读和队列背压影响，是本程序实际解码速度，不是设备声明帧率。
各路FPS可用相邻vN_frames差除以wall_us差计算。首个启动窗口和最后收尾窗口应单独分析。

```bash
# 新建结果目录，生成比运行时间长1秒的动态视频/音频；四个独立输入会话持续处理。
python3 scripts/run-stability.py --program build/release/esports_live \
  --directory build/my-soak --seconds 120 --width 1920 --height 1080 --fps 30

# perf软件采样，不要求支持cycles/instructions硬件计数器。
python3 scripts/run-stability.py --program build/release/esports_live \
  --directory build/my-profile --seconds 10 --width 1920 --height 1080 --fps 30 --perf

ctest --test-dir build/debug -R 'metrics_integration|video_reference|output_integration' --output-on-failure
```

脚本保存source.mp4、command.json、run.log、metrics.csv、sync.csv、FLV、MP4分段和summary.json；
启用perf时另保存perf.data及文本报告。检查每路输入帧数、输出帧数、队列上限、
同步报告收尾、录制完成和每段实际解码；发生错误退出非零，保留现场。
同一动态样片被四路分别解码，不代表四个独立摄像机或网络故障模型。
素材生成和最终解码验证不计入被测进程运行时长，profile会引入额外采样开销。

可增大 --seconds，最大86400；24小时素材和输出需要预估磁盘空间。
脚本不自动清理既有结果、不自动部署服务端，也没有运行磁盘轮转和容量管理。
RSS首尾窗口差只描述本次运行，不能据此证明无泄漏；ASan也只能覆盖执行到的路径。
当前实际测量及perf限制见测试报告，不能把短时验收等同24小时目标达成。

同尺寸YUV420P视频转换现在使用av_frame_ref共享只读像素，避免每帧重新分配和复制。
修改像素前必须av_frame_make_writable，修改时间戳只改独立帧头。
不同尺寸/格式仍使用swscale，不引入硬件编解码或新的技术路线。


### 2026-10-30：压力工具退出保护

压力脚本通过 scripts/process_runner.py 为每次外部命令创建独立进程组。
超时或Ctrl+C时，先向组内发送SIGINT，默认等待5秒供媒体写尾，再发送SIGKILL；
即使perf包装器先退出，也会检查后代是否停止执行，而不只等待包装器。
原超时/中断仍以失败结束，不能因子进程温和退出0而伪报测试成功。
强制终止不能保证当前媒体文件可恢复；不可中断内核I/O仍可能无法及时终止，会报告错误。
工具不跟踪自行setsid脱离进程组的任意第三方程序，仅面向本项目和FFmpeg/perf链路。

ffprobe与perf报告也有超时；各路音频解码块数必须覆盖生成样片目标时长，
不再以“音频曾经有帧”作为持续输入的充分条件。该计数规则针对脚本生成的48kHz/AAC素材。
新增测试包含包装器退出、忽略SIGINT的后代、Ctrl+C以及真实四路媒体温和超时收尾。
这补齐了测试工具的退出保护，不代表真实磁盘写满或24小时测试已经完成。


### 录制写失败与文件大小上限（2026-10-30）

新增recording_write_failure用例，在独立测试进程设置RLIMIT_FSIZE=4096，
让普通MP4文件触发真实内核EFBIG；另一支路实际封装FLV到管道，由父进程接收校验。
录制失败后不登记失败段，主FLV的100个视频包与该次分流前编码包逐包哈希一致并可完整解码。
这验证文件写入错误隔离，不等同于磁盘耗尽ENOSPC、断电或真实RTMP验收。

esports_live忽略SIGXFSZ，允许write返回EFBIG并走FFmpeg错误传播路径，
避免文件上限直接终止整个进程。录制错误仍按既有规则关闭支路并报告；
主直播输出文件写失败则退出1，不能把它当作直播成功。
该信号策略当前用于esports_live，不改变其他独立历史阶段程序。


## 源码阅读

从[中文代码阅读路线](docs/code-reading-guide.md)开始，再看
[src/live_main.cpp](src/live_main.cpp)和[src/manager/live_pipeline.cpp](src/manager/live_pipeline.cpp)。
主直播流程已分段标注；源码/头文件说明了模块职责、线程边界、FFmpeg资源所有权和时间戳单位，
测试和脚本也补充了验证目的。旧阶段入口用于逐步学习，src/main.cpp仅做环境检测。

## JSON配置启动（2026-10-30）

```bash
# 在项目根目录执行，首次使用先运行前文的mix/audio素材生成脚本。
./build/debug/esports_live --config config/server.json --check-config
./build/debug/esports_live --config config/server.json

# 查看合并后的有效配置，不运行媒体链路。
./build/debug/esports_live --config config/server.json --fps 30 --dump-config
```

优先级：代码默认值 → JSON → 命令行；命令行输入列表替换JSON列表，逐路参数按索引覆盖。
相对路径以当前工作目录为准；示例运行3秒并生成FLV、MP4分段和CSV，输出文件不能已存在。
JSON仅支持esports_live，不指定--config时保持旧CLI行为。详细字段表、禁用方式和校验边界见
[完整配置文档](docs/configuration.md)。当前没有配置热更新。

配置示例已逐项补齐：

- [36个配置项的示例与含义](docs/configuration.md#字段默认值和命令行映射)。
- [完整配置示例](config/server.full-example.json)：包含所有字段、定时布局切换和增益调整。
- [RTMP/RTSP输入及RTMP输出示例](config/server.network-example.json)：替换占位地址后使用。
- [场景运行与覆盖命令](docs/configuration.md#可直接加载的场景示例)：含四宫格、本地离线输出及禁用录制的方法。

示例是标准JSON，不能直接写注释；详细说明放在配置文档中。原有server.json保持不变。
