# Esports Live Server

基于Ubuntu/WSL2、C++11、CMake和FFmpeg C API的多路音视频制作程序。
代码已收敛为当前完整实现：输入 → 解码 → 画面合成/音频混音 → H.264/AAC编码 → RTMP或本地FLV，同时支持分段录制。

首次部署请从[完整部署指南](docs/deployment-guide.md)开始：依赖安装、编译测试、独立安装、本地验收、真实流配置、systemd托管及升级回滚。

## 程序入口

| 程序 | 用途 | 源码入口 |
|---|---|---|
| esports_live | 多源合成混音、同步、推流、并行录制及监控 | src/live_main.cpp |
| esports_replay | 录像关键帧查询、片段提取、实时回放 | src/replay_main.cpp |
| ffmpeg_env_check | FFmpeg六库版本与注册能力诊断 | src/env_check_main.cpp |

录屏、摄像头与主持人麦克风的具体场景，见[基础音画同步方案](docs/screen-camera-microphone-sync.md)，包含校准原因、偏移示例和当前实现边界。

第一次读这个项目，先跟着[一步步读代码](docs/first-code-walkthrough.md)：从`src/live_main.cpp`开始，每步给出搜索词、跳转位置、停止点和自检答案。

需要逐文件查询时，看[完整代码阅读指南](docs/code-reading-guide.md)：逐项解释全部46个头文件/实现文件（含测试和第三方头文件），
并提供调用图、视频/音频追踪、时间戳实例、线程所有权和GDB观察点；系统概览见[当前架构](docs/architecture.md)。
自有的45个头文件、实现和C++测试文件已补充初学者中文注释：文件导读、接口约定、关键分支、时间单位和资源交接。
建议先读文件顶部导读，再顺着函数旁的解释阅读；第三方PicoJSON保持上游原样，其使用边界见配置模块和阅读指南。
完整节目只有一个调度入口：src/manager/live_pipeline.cpp。LiveOptions独立定义，不再继承阶段配置。
已删除早期独立转封装、单路转码、纯视频演示入口及专用实现；历史版本可通过Git查看。

本地功能与故障路径已有测试，真实RTMP/RTSP成功会话、实际播放、端到端延迟及24小时稳定性仍待验收。
“代码收敛”不代表生产验收全部完成。不部署SRS，不实现WebRTC。

## 环境与构建

已测Ubuntu 24.04/WSL2、GCC/G++ 13.3、CMake 3.28、FFmpeg 6.1.1。
强制C++11，pkg-config查找avformat/avcodec/avutil/avfilter/swresample/swscale，Threads链接线程依赖。

```bash
sudo apt install build-essential cmake pkg-config gdb git python3 ffmpeg \
  libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev \
  libswresample-dev libswscale-dev

cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel 4
ctest --test-dir build/debug --output-on-failure

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure
```

编码需要libx264和AAC；测试生成素材还需libx265/libopus。Python及FFmpeg CLI只用于测试和素材生成，
核心处理直接调用FFmpeg C API。可用-DBUILD_TESTING=OFF只构建运行工具。

```bash
# 安装到独立的新目录；bin只包含上表三个工具，不含测试夹具。
cmake --install build/release --prefix "$PWD/build/delivery"
```

安装配置位于share/esports-live-server/config；其中素材路径仍相对于启动工作目录，使用时按实际部署修改。
已有旧build目录可能残留已取消目标的二进制；CMake不再构建它们，交付以新安装目录为准。

## 快速开始

从项目根目录执行。素材生成脚本拒绝覆盖，已生成时跳过前两条：

```bash
bash scripts/generate-mix-samples.sh
bash scripts/generate-audio-samples.sh
./build/debug/ffmpeg_env_check
./build/debug/esports_live --config config/server.json --check-config
./build/debug/esports_live --config config/server.json
```

示例运行3秒，生成本地FLV、MP4分段和CSV。输出父目录必须存在，输出文件必须尚不存在；
再次运行请修改output、record_prefix、stats_file、metrics_file或用CLI覆盖为新路径。
Ctrl+C触发停止和收尾。

| 配置 | 用途 |
|---|---|
| [server.json](config/server.json) | 本地双画面、两路音频、录制和监控 |
| [server.full-example.json](config/server.full-example.json) | 全部36个字段及定时布局/增益变化 |
| [server.network-example.json](config/server.network-example.json) | RTMP/RTSP输入和RTMP输出；须替换占位地址 |

[配置说明](docs/configuration.md)包含逐字段示例、默认值、范围和CLI映射。
优先级为默认值→JSON→CLI；--dump-config导出有效值，--check-config只校验参数，不验证媒体或网络。
JSON不支持注释、热更新或环境变量展开。

## 推流与输入

直播需要2或4路视频，支持本地文件、RTMP拉流、RTSP TCP摄像头；视频H.264/H.265，音频AAC/Opus。
audio_inputs省略或为空时继承视频URL。没有可用音轨时对应源降级静音，节目仍输出AAC。
向已有RTMP接收端推流的例子：

```bash
./build/debug/esports_live --config config/server.json \
  --output "rtmp://你的服务器/live/esports" --realtime \
  --record-prefix "" --stats-file "" --metrics-file ""
```

本程序不是RTMP接收服务器，OBS不能直接推入本进程。持续网络节目设duration_ms=0；本地文件不会自动循环。
输入失败后降级黑帧/静音，尚无输入自动重连。输出有超时、有限重连、退避和新IDR恢复。
音视频分别打开源URL，真实设备连接数和时钟对齐需要实测。

## 录制与回放

esports_live 可增加独立录制支路，沿用已经编码的 H.264/AAC，不重复编码。
录制队列满或录制错误时记录 ERROR 并停用录制，直播继续；程序整体退出0不等于录制成功，
需检查“录制完成”日志和索引。索引只登记已成功写尾的段，不登记正在写入的段。

```bash
# 先用快速开始中的脚本生成 mix/audio 素材；所有输出路径必须尚不存在。
./build/debug/esports_live \
  --input build/samples/mix/red.mp4 --input build/samples/mix/lime.mp4 \
  --audio-input build/samples/audio/game.m4a --audio-input build/samples/audio/voice.opus \
  --output build/samples/demo-live.flv \
  --record-prefix build/samples/demo-record --record-format mp4 --segment-ms 1000 \
  --duration-ms 3000 --width 320 --height 240 --fps 25

# 查看节目时间到文件的映射，以及某段的关键帧位置。
cat build/samples/demo-record.index.tsv
./build/debug/esports_replay --input build/samples/demo-record-000001.mp4 --list-keyframes

# 按节目索引定位；1.2秒请求向前定位到所在段的1.0秒IDR。
./build/debug/esports_replay --index build/samples/demo-record.index.tsv \
  --start-ms 1200 --duration-ms 500 --output build/samples/demo-highlight.mkv

# 按DTS实时回放；也可把output换成已有RTMP接收地址。
./build/debug/esports_replay --input build/samples/demo-highlight.mkv \
  --mode replay --output build/samples/demo-replayed.flv
ffmpeg -v error -xerror -i build/samples/demo-replayed.flv -f null -
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


## 监控与压力测试

esports_live支持 --metrics-file 新文件.csv，与 --stats-file 同时使用可分别观察进程资源和同步/输出。
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


## 验证与限制

当前CTest共16项，全部验证保留的运行工具或其共用模块：环境、日志、进程清理、视频引用、配置、
指标、输入故障、编解码、布局、混音同步、帧/包队列、输出恢复、录制回放及写失败隔离。

```bash
ctest --test-dir build/debug -R 'codec_integration|video_layout_integration|input_integration' --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -no-pie"
cmake --build build/asan --parallel 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan --output-on-failure
```

VSCode只提供完整直播、录像提取、环境检测三个调试选项；Ctrl+Shift+B构建，F5启动。
回放调试使用直播调试生成的config-debug-record-000000.mp4，需要先完成直播录制。

真实网络成功推流/播放和重连、人工音画检查、24小时运行、真实磁盘写满与断电恢复仍待验收。
已测EFBIG录制写失败隔离，不等于ENOSPC或断电验收；短时压力测试不证明长期无泄漏。
诊断CSV写失败目前会终止节目，录像回放不切入当前节目，也不支持跨分段自动拼接。

当前源码与使用说明以本README、配置说明、架构和阅读路线为准。
[开发计划](docs/development-plan.md)、[学习笔记](docs/learning-notes.md)和[测试报告](docs/performance-report.md)
保留有日期的历史记录，旧阶段命令不再作为当前运行入口。
