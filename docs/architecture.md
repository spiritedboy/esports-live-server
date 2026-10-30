# 架构说明

当前已实现到 Phase 4；下文按阶段记录架构演进。

## Phase 0 + Phase 1：基础与 Remux

```text
esports_remux（参数、信号）
  └─ run_remux（单线程生命周期和实时节奏）
      ├─ NetworkRuntime（网络全局初始化/反初始化）
      ├─ IoControl（停止标志、每次 I/O 的截止时间）
      ├─ StreamInput → AVPacket
      └─ StreamOutput → 本地 FLV 或 RTMP
```

esports_common 提供 Logger；esports_media 提供三个媒体模块。
ffmpeg_env_check 保留为独立工具，logger_test 与媒体集成测试分开。

## 数据流与所有权

StreamInput 独占输入 AVFormatContext，析构调用 avformat_close_input。
AVStream/codecpar 由该上下文持有，输出用 avcodec_parameters_copy 复制参数。
AVPacket 结构由 unique_ptr + av_packet_free 管理，循环读取前 unref；
av_interleaved_write_frame 消耗包的缓冲引用，调用后即使错误也不重试原包。
没有 AVFrame 或编解码上下文。

StreamOutput 独占输出上下文、I/O 上下文及本地文件描述符。
本地文件用 O_EXCL 原子创建，经自定义 AVIO 写入，禁止覆盖旧文件和符号链接。
RTMP 使用 avio_open2。正常结束显式 finish 写尾并检查错误；
析构仅清理资源，不隐式执行可能抛异常的写尾。
自定义 AVIO 当前 buffer 用 av_freep 释放，然后释放 AVIOContext；
RTMP I/O 用 avio_closep，最后释放输出 AVFormatContext。
IoControl 的生命周期覆盖两个上下文；NetworkRuntime 最后销毁。

## 时间轴和流选择

每种音视频最多一路，允许只有音频或只有视频，其他非音视频流忽略。
H.264/AAC 之外的音视频流明确拒绝。mapping 将输入索引映射为输出索引。
第一个选中包的 DTS 换算为微秒，作为所有流共用原点；分别换回输入时间基后
同时从 PTS/DTS 减去原点，再转换到写头后的输出时间基。
封装器处理剩余负时间戳，不独立把各路流归零。检测缺失时间戳和非递增 DTS。
同一来源的音视频相对偏移得以保留；这不等同于多设备时钟同步。

实时发送比较媒体 DTS 经过时间和 av_gettime_relative 单调经过时间，
每次最多睡眠 10ms，以便响应停止。默认启用，可用 --no-realtime 关闭。

## 线程和失败处理

当前只有调用线程，无自建媒体队列，不使用 detach。
FFmpeg 内部交织等待设置 max_interleave_delta=500000 微秒；
这不是硬性字节内存上限或实时调度保证。
信号处理器仅写 sig_atomic_t，不输出日志或释放资源。
FFmpeg 阻塞操作通过 AVIOInterruptCB 检查用户停止和操作截止时间。
本地文件 EOF 正常写尾；网络 EOF 报告连接结束；错误向入口传播并记日志。
停止后允许有超时限制的写尾，异常清理日志不覆盖最初错误。
本地文件写入错误通过 AVIO 错误码传播。内核级磁盘阻塞不保证可中断。

## 后续边界

完整目标见 development-plan.md。Decoder、Mixer、队列和多源同步尚未实现。
自动重连与恢复、低延迟优化、录制回放按后续阶段推进。

## Phase 2 扩展：单路编解码

上文“当前实现”描述保留 Phase 1 的结构；现在额外有以下流水线：

```text
StreamInput → MediaDecoder → AVFrame
  ├─ VideoConverter（sws）→ H.264 MediaEncoder ─┐
  └─ AudioConverter（swr + 有界 FIFO）→ AAC ───┤
                                             └→ StreamOutput → FLV/RTMP
```

MediaDecoder/MediaEncoder 独占 AVCodecContext。回调接收 move 的 Frame/Packet，
不将复用的裸指针跨越所有权边界。编码器输出描述通过 parameters_from_context
复制给输出模块，沿用 Phase 1 的文件保护、I/O 中断和封装逻辑。
单路转码保持视频 PTS，以源统一起点建立音频采样时间轴；不实现漂移修复。
音频 FIFO 最多 192000 采样，结束时逐级排空，最终写 trailer。

## Phase 3 扩展：多路视频

```text
VideoSource 0：输入/解码/像素转换 → FrameQueue(8) ─┐
VideoSource 1：输入/解码/像素转换 → FrameQueue(8) ─┤
VideoSource 2：输入/解码/像素转换 → FrameQueue(8) ─┤
VideoSource 3：输入/解码/像素转换 → FrameQueue(8) ─┘
    主线程：CFR 调度 → VideoMixer(filter graph) → H.264 Encoder → StreamOutput
```

每个 VideoSource 的 FFmpeg 上下文由其工作线程独占。
主线程只通过有锁 FrameQueue 和原子失败/停止状态与工作线程通信。
Logger 共用同一实例，生命周期覆盖所有工作线程。
每路队列最多 8 帧，生产者满时阻塞；主线程选择当前时刻之前最新帧，保留未来帧。
FrameQueue 传递 unique_ptr<AVFrame>，滤镜输入另外创建引用；主线程更新最近帧
不会使滤镜持有的缓冲失效。每条链路只持有有限帧数，但不是进程 RSS 的硬性上限。

各输入首视频时间戳归零；调度帧 k 的统一媒体时间为 k/fps。
VideoMixer 的 buffer 时间基为 1/fps，scale 后通过 hstack/xstack/overlay 合成，
buffersink 每轮取出一帧；再换成编码器 1/90000，最终转成封装器 1/1000。
图中所有输入具有相同输出 tick，避免滤镜的多路同步无限等待缺失输入。
布局切换重建滤镜图，编码器和输出流不重建，PTS 连续。

每路保留最后帧至 stale_ms，超时后传黑帧。失败源关闭队列并记录警告，
其他源继续。初始全部无帧则不创建输出。正常文件全部读完后按最长源结束。
显式 duration_ms 可继续输出至指定时间，即使后期只有黑帧。

实时模式不在每个 tick 等待慢源；离线模式有限等待源提供未来帧，确保测试可复现。
队列背压保证内存不随节目时长无限增长，但没有实现低延迟丢包/重连策略。
退出时先向所有源发送原子停止并关闭队列，再逐一 join，最后释放其余上下文。
工作线程继承屏蔽的 SIGINT/SIGTERM，直接操作媒体的线程通过原子取消标志退出。

Phase 3 没有音频混音/透传，不生成虚构音频轨。多设备时钟漂移与音画同步策略
留在 Phase 4；真实 RTMP 会话及长期稳定性尚未验收。

## Phase 4 扩展：音视频共同调度

新增 esports_live，旧 esports_composite 保持仅视频兼容行为。

- VideoSource：共同容器原点、可配置视频偏移、SourceClock 异常处理；8 帧有界队列。
- AudioSource：AAC/Opus 解码、独立输入连接、AudioResampler 漂移补偿；32 块有界队列。
- AudioQueue：按精确采样区间读取，支持跨块消费，缺口静音、过期块统计。
- AudioMixer：逐路增益/静音、5ms 平滑过渡、双声道共同峰值限制。
- run_live：48kHz 采样主时钟，按有理数时间比较调度视频与音频事件。
- StatsFile：排他创建 CSV，记录媒体末端差和墙钟进度；不是内容同步测量器。

所有输入工作线程通过原子停止标志、队列关闭和 join 退出。
视频/音频解码上下文不跨线程调用，源帧通过 unique_ptr 转移，混音窗口使用独立缓冲。
主线程拥有视频滤镜、音频混音状态、两个编码器和输出封装上下文。
资源失败时尽力写尾；所有源启动时均无媒体则报错，不生成虚构正常节目。

每个音频源的 SWR 保持连续输出采样计数，结合内部滤波延迟计算相位误差。
小误差软补偿，大间断重新定位；缺失/回退/重复/超大跳变由 SourceClock 重建 epoch。
已对齐的每路 PCM 窗口拥有相同 PTS/样本数，混音不会把不同时间片直接相加。
正常 EOF 排空源解码与重采样，然后主线程按最长源时间收尾并排空 H.264/AAC 编码器。
显式 duration_ms 可在所有源结束后继续输出黑帧和静音，直到节目时长结束。

同源音视频使用独立连接，文件起点可复现；网络起点、设备时钟关系需实际验证。
没有自动 NTP 对齐、动态音频参数切换、网络重连或长时间稳定性声明。


## Phase 5 扩展：输出线程与恢复状态

本节更新 Phase 4 的输出所有权：主线程保留合成/混音/编码，封装上下文转移到独立输出线程。

```text
VideoSource（每路4帧） ─┐
                       ├→ run_live 合成/混音/编码 → PacketQueue → AsyncOutput → StreamOutput
AudioSource（每路16块）─┘                           有界/可关闭    独占FFmpeg输出
```

输入队列容量可配置。PacketQueue 同时约束包数、数据字节和网络媒体跨度；
本地模式只按包数/字节背压。网络清空递增 epoch、请求 IDR，旧窗口失效。
恢复状态为等待IDR → 打开并写头 → 发送 → 错误/积压 → 有限退避 → 等待新IDR。
若握手后尚未发送媒体，允许保留连接并等待新鲜IDR；已发送媒体的旧连接则销毁。
新的会话统一减去首IDR的DTS，所有音视频共同平移，保持相对位置。
编码参数在启动前复制，输出线程不访问主线程的编码器。

取消：原子停止 → 关闭队列并唤醒 → AVIO回调中断/结束退避 → 尽力写尾 → join。
失败：保存 exception_ptr → 关闭队列 → 主线程 check/finish 重抛 → 所有输入线程退出。
正常EOF：排空编码器 → 关闭生产端 → 输出线程排空 → 写尾 → join。
未恢复且已无新IDR时退出失败。测试用 PacketSink 可替换传输边界，生产始终使用 FFmpeg。

仅 esports_live 使用此链路；输入流自动重连未实现，失败源仍按此前策略黑帧/静音降级。
Phase 5 结束时尚未实现录制与回放，后续扩展如下。


## Phase 6 扩展：编码包分流与录制隔离

```text
                      ┌→ AsyncOutput → FLV/RTMP直播
合成/混音 → 编码回调 ─┤
                      └→ av_packet_clone → Recorder队列 → MP4/MKV分段 → TSV索引

TSV节目时间定位 → 文件内av_seek_frame → 关键帧起点 → 提取/实时回放输出
```

Recorder只有一个生产者和一个消费者；队列默认16MiB、最多1024包，满时关闭录制，
主链路不等待可写条件。线程独占封装器、文件及索引，主线程拥有编码器。
两支路使用独立AVPacket头和共享只读压缩缓冲。编码参数启动前快照，不跨线程访问编码器。

切段在新IDR写入前发生，旧段写尾成功后登记绝对文件路径和节目时间半开区间。
新段用首IDR微秒DTS作为音视频共同原点，保持时间差，丢弃原点之前迟到的AAC预卷。
索引定位只选择已经登记的段，损坏、越界、跨段请求明确失败。
提取与回放复用StreamInput/StreamOutput，不通过shell调用FFmpeg来实现核心功能。

生命周期：主线程停止提交 → 关闭录制队列 → 排空有效包 → 写尾及索引 → join。
录制故障：标记failed → 关闭/清空队列 → 报告ERROR → 直播继续，当前坏段不登记。
文件保护覆盖索引和媒体路径；正常MP4不保证强杀可恢复，索引不保证断电持久性。
当前不支持跨分段拼接、采样级无缝切段或回放切入正在运行的节目；Phase 6结束时尚未实施Phase 7，后续扩展如下。


## Phase 7 扩展：监控与诊断

- CodecMetrics：原子帧/包计数及API耗时，消费回调不在计时范围内。
- VideoSource/AudioSource：持有解码统计，生命周期长于解码工作线程；主线程只读原子快照。
- MetricsFile：主线程按秒写性能CSV，getrusage读取进程CPU和峰值RSS，proc读取当前RSS/线程数。
- run_live：读取每路输入统计、当前队列长度、输出统计，追加混音/合成累计耗时和调度跳过量。
- run-stability.py：独立测试工具，生成真实持续输入、运行节目、检查包数和产物解码、保留结果。

同格式同尺寸VideoConverter输出av_frame_ref引用，独立帧头、共享只读像素；
解码器和滤镜按引用计数持有缓冲，后续修改像素必须make_writable。
现有队列最大容量及线程退出顺序保持，原有超时、恢复、录制失败隔离测试纳入回归。

监控CSV读写不是无开销，指标文件失败按明确异常退出；生产部署如需遥测失败隔离
应另行定义策略。CPU调用计时不等于后台线程CPU总量，队列快照不是全局事务快照。
perf当前环境缺少完整符号/调用栈支持，短时结果及未验收项见performance-report。


### 压力工具的进程边界（2026-10-30）

run-stability通过process_runner为素材生成、直播/perf包装、ffprobe和结果解码建立
独立进程组。超时/中断路径为组SIGINT → 有限写尾宽限 → SIGKILL → 有界检查后代停止。
正常媒体线程模型不变，工具失败不被当作程序验收通过；不操作组外进程。


录制写入失败补测使用RLIMIT_FSIZE触发内核EFBIG，主测试支路为真实FLV管道封装。
esports_live将SIGXFSZ转换为常规write错误处理机会（忽略信号、仍检查返回错误）；
失败MP4不登记索引，主输出失败则退出1，录制失败仅关闭录制支路。不是ENOSPC或断电验收。


### 启动配置层（2026-10-30）

C++默认值 → load_live_config(JSON) → 显式CLI覆盖 → 统一参数校验 → run_live。
新增esports_config目标，只供esports_live链接，PicoJSON头文件为目标私有依赖。
配置读取发生在线程/媒体资源创建之前，check/dump分支校验后直接退出。
输入列表整体替换，逐路参数按索引覆盖；相对路径以当前工作目录为准。
没有新增热更新、网络服务或另一套FFmpeg处理链路，字段及边界见configuration.md。
