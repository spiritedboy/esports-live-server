# 当前系统架构

本文描述2026-11-09收敛后的完整实现。主程序为esports_live，独立录像工具为esports_replay，
ffmpeg_env_check只做环境诊断。生产代码不再包含阶段演示流水线。

## 数据流与线程

```text
JSON/CLI → LiveOptions → run_live（主处理线程）

每路VideoSource：StreamInput → MediaDecoder → VideoConverter → FrameQueue
每路AudioSource：StreamInput → MediaDecoder → AudioResampler → AudioQueue
                                                              ↓
主处理线程：节目时钟 → 选视频帧/取音频窗口 → VideoMixer/AudioMixer → MediaEncoder
                                                              ↓
                                      ┌→ PacketQueue → AsyncOutput → FLV/RTMP
                                      └→ Recorder队列 → MP4/MKV分段与TSV索引

esports_replay：本地文件/索引 → av_seek_frame → 关键帧对齐 → StreamOutput
```

每个视频/音频源各有一个输入解码线程，即使URL相同也分别打开。主线程独占合成、混音和编码器。
输出线程独占网络封装器，录制线程独占录制文件和索引。编解码器内部可能另有FFmpeg线程。
没有为每个C++模块额外创建线程，消费回调是同步调用。

## 模块职责与所有权

| 模块 | 输入 → 输出 | 资源与生命周期 |
|---|---|---|
| StreamInput | 文件/URL → AVPacket | 独占解复用上下文，借用IoControl，读取前unref旧包 |
| MediaDecoder | AVPacket → AVFrame | 借用输入包，移动交付输出帧，EOF送nullptr排空 |
| VideoConverter | 解码视频 → 统一尺寸YUV420P | 同尺寸同格式av_frame_ref；其他情况swscale |
| AudioResampler | 解码音频 → 48kHz双声道FLTP | swresample转换及小幅漂移补偿；大间断留静音缺口 |
| FrameQueue/AudioQueue | 输入线程 → 主处理线程 | 有界、互斥锁保护、close唤醒等待者 |
| VideoMixer/AudioMixer | 同一节目时刻内容 → 合成/混音帧 | 主线程独占，借用输入帧，输出交付编码器 |
| MediaEncoder | 帧 → H.264/AAC包 | 编码上下文由主线程独占，输出参数提前快照 |
| AsyncOutput | 编码包 → FLV/RTMP | 接管Packet，独立工作线程执行写入与有限恢复 |
| Recorder | 编码包引用 → MP4/MKV/索引 | 独立包外壳共享缓冲；队列满或写失败关闭录制 |
| StreamOutput | 包和时间基 → 封装数据 | 复制参数，写头后使用输出时间基，显式finish写尾 |

Packet/Frame的unique_ptr管理外壳，FFmpeg缓冲区引用计数管理媒体数据。
跨线程移动所有权；两支路需分别修改PTS/DTS时，使用独立外壳。
共享视频像素只读，修改前须av_frame_make_writable。
av_interleaved_write_frame会消耗包引用，失败后不再尝试写同一个包。

## 统一节目时间轴

输入首先减去容器共同原点，保留同容器音视频偏移；无起点时回退首帧并记录限制。
视频队列单位为微秒，音频队列单位为1/48000秒。SourceClock重建缺失、回退或大跳变时间戳，
但不推断不同设备的物理时钟关系；用户可配置逐路毫秒偏移。

主循环用tick/fps和cursor/48000比较两轨下一处理时刻，先处理较早一轨。
视频选不晚于当前时刻的最新帧，短缺沿用最近帧，超stale_ms切黑；
音频为同一采样区间预填静音，再覆盖可用PCM。音频增益有5ms过渡，混音使用双声道联动峰值限制。
实时节目按单调时钟等待，离线处理允许有限等待输入，音频采样位置按实际样本数推进。

合成滤镜time_base为1/fps，H.264编码time_base为1/90000，AAC为1/48000。
编码包进入封装器后再换算为写头确定的AVStream::time_base。音视频共用一个原点，不能分别把首包归零。

## 背压、输出恢复与停止

输入视频队列默认每路4帧、音频16块。容量满时输入线程等待，关闭队列同时唤醒生产者与消费者。
输出包队列限制数量、字节；实时网络还限制媒体时间跨度。本地模式背压保数据，
网络模式丢弃整批过期窗口、递增epoch并请求新IDR。已出队的包写前也要核对epoch。

网络输出恢复：等待IDR → 打开连接/写头 → 发包 → 失败或过期 → 有限退避 → 新IDR。
重试预算是生命周期累计值，退避可取消。握手成功但尚未发媒体时允许等待更新IDR；
已经发出旧窗口的连接需要销毁重建。新会话以首IDR DTS为音视频共同原点，舍弃此前AAC预卷。
主线程调度落后超过预算时可跳过旧节目位置并请求IDR，避免追发大量历史数据。

正常EOF排空编码器、关闭生产端、排空输出及录制队列、写尾、join。
取消设置原子标志、关闭队列、唤醒退避、通过AVIO回调中断I/O，再回收工作线程。
输入源异常关闭队列并降级；输出异常保存exception_ptr，由主线程check/finish重抛；录制异常仅停用录制。
IoControl写尾模式忽略用户停止但保留超时；自定义本地文件write不承诺能中断任意内核磁盘阻塞。

## 文件保护、录制与回放

自定义文件AVIO通过O_EXCL排他创建，禁止覆盖已有路径及符号链接；检查write、flush和trailer阶段错误。
录制队列最多1024包、默认16MiB，submit不等待慢磁盘。目标时长到达后等下一IDR切段，
旧段写尾成功才登记TSV半开区间[start_us,end_us)与绝对路径。切段使用共同原点，AAC预卷可能被丢弃，
不保证采样级无缝；索引不保证断电持久性，普通MP4被强杀后当前段可能不完整。

回放工具限定本项目无B帧H.264/AAC录像，向前定位关键帧，快速提取或按DTS实时发送。
索引正时长请求不允许跨段，duration_ms=0只到所选文件末尾。回放不切入正在制作的节目，不自动重连。

## 配置、监控与验证边界

启动顺序：C++默认值 → 严格JSON加载 → CLI覆盖 → 统一校验 → 创建媒体资源。
LiveOptions独立定义完整节目参数；JSON仅生成参数，复用入口校验。PicoJSON是私有第三方依赖。
check/dump不会访问媒体或网络，相对路径以进程工作目录为准，没有配置热更新。

CodecMetrics原子累计帧数及API耗时，MetricsFile汇总CPU/RSS/FPS与队列；StatsFile记录节目同步和输出统计。
指标快照不保证全局同时刻，API耗时不等于全部后台线程CPU时间。CSV写失败会终止节目。
压力工具单独生成素材并管理测试进程组，超时先SIGINT再SIGKILL，并检查后代停止。

测试夹具只用于故障注入，生产默认使用真实FFmpeg。真实网络成功会话、设备同步、端到端延迟、
长期RSS和24小时稳定性仍待验收；整理交付代码不改变这些边界。
