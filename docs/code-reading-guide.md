# 代码阅读路线

本文对应2026-10-31的实现。先读一条完整数据流，再深入各模块；不用一开始就逐行读完参数解析和CSV格式化。

## 1. 从哪个入口看

完整直播从[src/live_main.cpp](../src/live_main.cpp)进入。
它先把JSON和命令行合并成[LiveOptions](../include/manager/live_pipeline.h)，校验后调用
[src/manager/live_pipeline.cpp](../src/manager/live_pipeline.cpp)中的`run_live`。
`--check-config`和`--dump-config`在调用流水线之前退出，不打开媒体。

`src/main.cpp`仅是环境检测入口，不是直播主程序。早期程序仍保留为独立学习路径：

| 程序 | 入口 → 核心实现 | 适合先理解什么 |
|---|---|---|
| esports_remux | remux_main.cpp → manager/remuxer.cpp | 读包、时间基换算、转封装 |
| esports_transcode | transcode_main.cpp → manager/transcoder.cpp | 解码、转换、编码以及EOF排空 |
| esports_composite | composite_main.cpp → manager/compositor.cpp | 输入线程、选帧和视频合成 |
| esports_live | live_main.cpp → manager/live_pipeline.cpp | 完整音视频节目与异步输出 |
| esports_replay | replay_main.cpp | 关键帧定位、片段提取与实时回放 |

## 2. 顺着主直播循环看

`run_live`已用①～⑦标注阅读段落：

1. **创建输入源**：每个VideoSource/AudioSource构造后启动一个工作线程。主线程有限等待首批媒体或源失败状态。
2. **创建处理和输出对象**：黑帧、视频/音频编码器、合成器、异步输出和录制。
3. **连接编码输出回调**：录制先clone包引用，推流取得原Packet的所有权；避免两路封装修改同一个包外壳。
4. **推进节目时钟**：比较下一视频帧和下一音频块的时间，先处理较早者。输入源PTS用于选内容，节目时钟决定输出位置。
5. **视频分支**：从各源队列取不晚于当前时刻的最新帧；缺帧先沿用最近画面，超过阈值改用黑帧，然后合成和编码。
6. **音频分支**：创建同一采样窗口并预填静音，覆盖各源实际PCM，调增益、混音、限幅后送AAC编码器。
7. **收尾**：正常结束排空编码器和输出队列；用户停止时优先取消阻塞输出。输入源先stop再join，析构保证异常路径也回收线程。

注意：`video_packet`、`audio_packet`、`decoded`等lambda只是同步调用的回调，不意味着它们各有一个线程。
判断执行线程要从调用者往上追，而不是看到回调就假定异步。

## 3. 各线程拿什么数据

```text
视频输入线程：StreamInput → MediaDecoder → VideoConverter → FrameQueue
音频输入线程：StreamInput → MediaDecoder → AudioResampler → AudioQueue
                                                          ↓
主处理线程：选帧/采样窗口 → VideoMixer/AudioMixer → MediaEncoder
                                                          ↓
                                  ┌── PacketQueue → AsyncOutput工作线程
                                  └── 独立录制队列 → Recorder工作线程
```

同一URL的音视频目前可能打开两次。每个FFmpeg上下文由其所属执行线程操作，跨线程传递帧/包，
不让两个线程同时调用同一解码器。FFmpeg内部还可能有自己的工作线程，与上面的应用层线程不同。

队列满时，视频/音频输入生产者等待，这是背压。关闭队列必须唤醒等待者，否则只调用join可能一直等不到退出。
输出包队列按用途区分：本地输出背压保数据，实时网络丢弃过期窗口并请求新IDR。
录制队列溢出则关闭录制支路，避免慢磁盘冻结主直播。

## 4. AVPacket和AVFrame到底归谁

| 代码动作 | 所有权及生命周期 |
|---|---|
| `make_packet()` / `make_frame()` | 创建外壳，由unique_ptr加FFmpeg专用deleter管理；此时不等于已有媒体数据 |
| `object.get()` | 临时借用裸指针，不转移所有权，不要另用unique_ptr接管 |
| `std::move(object)` | 把外壳所有权交给回调/队列；原指针不再拥有对象 |
| `av_packet_clone` / `av_frame_ref` | 新外壳或目标外壳取得缓冲引用，引用计数媒体缓冲可以共享，通常不复制大块数据 |
| `av_packet_unref` | 释放当前包携带的引用，保留外壳供下次read复用 |
| `av_packet_free` / `av_frame_free` | 释放引用和外壳，由RAII析构调用 |
| `av_interleaved_write_frame` | 消耗包的缓冲引用；失败后也不重试同一个包，外壳仍由调用者回收 |

同尺寸YUV420P视频可直接引用像素，原帧释放后新引用仍有效。修改新帧PTS只影响其外壳；
若要修改共享像素，先调用`av_frame_make_writable`。
录制与直播可以共享只读压缩数据，但必须拥有独立的包外壳，因为各自会修改PTS/DTS和stream_index。

## 5. 时间戳必须连同单位一起读

| 所在位置 | 时间单位/含义 |
|---|---|
| 解复用包PTS/DTS | 输入AVStream的time_base |
| VideoSource输出、FrameQueue | 微秒，已减原点并应用源偏移 |
| AudioSource输出、AudioQueue | 1/48000秒，即48kHz采样位置 |
| 主循环tick、视频合成滤镜 | 1/fps秒，即视频帧序号 |
| 主循环cursor、AAC编码器 | 1/48000秒 |
| H.264编码器 | 1/90000秒 |
| StreamOutput写包 | 写头之后的输出AVStream::time_base |
| queued_us、wall_start | 单调时钟微秒，用来计时和限速，不是媒体PTS |

公式是`时间（秒）= 时间戳 × time_base.num / time_base.den`。代码用`av_rescale_q`进行整数换算。
例如25fps时第25帧位于1秒，编码器PTS为90000，而音频cursor为48000；这些整数不能直接比较。
`av_compare_ts`同时接收两个时间基，主循环用它决定下一次处理视频还是音频。

PTS表示展示时间，DTS表示解码时间；有B帧时可能不同。输入可能有B帧，当前直播H.264编码关闭B帧。
封装器写头可能调整输出time_base，因此不能始终沿用编码器的time_base直接写包。
音视频减去同一个原点才能保留相对偏移；各自把首帧归零会抹掉偏移。
这仍不等于自动同步不同摄像头的物理时钟，真实设备关系需要测量。

## 6. 几个容易误读的控制流程

- **EAGAIN不是固定重试睡眠**：编解码send遇到EAGAIN应先receive释放输出空间，再重送输入；网络读取EAGAIN才是另一种等待场景。
- **EOF不等于没有尾部数据**：解码器、重采样器、FIFO、编码器可能都有缓存，必须沿链路顺序排空。
- **输出重连需要新IDR**：仅重新连上TCP还不够。旧P帧依赖之前的参考帧，新会话要写媒体头，并从新IDR和共同音视频原点开始。
- **epoch表示缓存代次**：队列reset后，消费者手里已经出队的包也可能过期；写入前核对代次，不能只检查队列是否为空。
- **录制成功以写尾为准**：创建了文件不等于生成了完整MP4。写尾成功后才登记索引；分段等待IDR，目标时长不保证精确。
- **统计不是全局原子快照**：各源计数独立采样；API耗时不等于所有编解码线程CPU总和，短时RSS稳定也不证明长期无泄漏。

## 7. 用测试对照代码

先看三个小测试理解约定，再看集成测试：

- `tests/video_reference_test.cpp`：引用计数、源释放和写时复制。
- `tests/frame_queue_test.cpp` / `packet_queue_test.cpp`：背压、关闭唤醒、关键帧和缓存代次。
- `tests/audio_sync_test.cpp`：采样窗口、限幅、漂移补偿；600秒是加速媒体时间。
- `tests/live_integration.py`：解码后的音视频内容是否对齐，不只看容器PTS。
- `tests/output_integration.py` / `recording_write_failure.py`：故障恢复与分支隔离的实际边界。

```bash
cmake --build build/debug --parallel 4
ctest --test-dir build/debug --output-on-failure
```

源码旁注释解释当前实现，不代表所有目标均已验收。真实网络成功会话、输入自动重连和长期稳定性等状态
以[开发计划](development-plan.md)为准。第三方PicoJSON保持上游内容不变，项目适配逻辑已在配置模块注释。
