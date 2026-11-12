# 第一次读这个项目：跟着一帧画面走

适用代码：2026-11-12工作树。本文是按顺序执行的阅读任务；每次只打开当前步骤要求的文件。

**第一个打开的文件是 `src/live_main.cpp`。先找 `main` 最后的 `run_live` 调用，不必从第一行逐行向下读。**

之前的[完整代码阅读指南](code-reading-guide.md)可以当字典。现在先跟本文走通一条路线，遇到某个模块想深入时，再查字典。
这里的“先略过”只是阅读顺序，不是让你删除代码或禁用功能。

## 先约定一种读法

在VSCode中按`Ctrl+P`输入文件路径，进入文件后按`Ctrl+F`搜索本文给出的函数名或代码词。
路径都从项目根目录算起。用搜索词定位，避免注释增加后行号改变。

每走到一个函数，只记录四件事：

| 要记录什么 | 例子 |
|---|---|
| 谁调用它 | 视频输入线程调用解码器 |
| 输入是什么 | 压缩视频包AVPacket |
| 输出是什么、交给谁 | 原始图像AVFrame交给回调 |
| 谁保管数据 | 回调得到Frame后移交队列 |

先把这四件事弄清楚，再研究每个FFmpeg参数。遇到库函数，先理解它在此处的用途，不必立即进入FFmpeg源码。

这次用“两个本地视频文件、左右拼接、输出一个本地FLV”的场景在脑中追踪。
两路音频仍正常处理，只是第一遍先沿视频路径阅读。文件名是场景说明，不要求你先准备素材才能读代码。

建议分四次读：第1～3步认识入口，第4～9步走通视频，第10～12步接上音频和退出，第13步以后再看扩展能力。

## 第1步：找到程序真正进入媒体处理的地方

打开[src/live_main.cpp](../src/live_main.cpp)，搜索`int main`，然后直接搜索：

```cpp
return esports::run_live(options, stop_requested, log);
```

这就是从“处理启动选项”进入“处理音视频”的交接点。它传入三个东西：

- `options`：输入地址、输出地址、尺寸、帧率、布局等最终配置。
- `stop_requested`：收到退出信号后置位，让媒体循环知道该停下了。
- `log`：记录运行信息的日志器。

现在向上找到`LiveOptions options`，知道这个配置对象是在main中创建的即可。
中间很长的`--width`、`--gain`、JSON合并和范围检查先折叠。它们的作用是保证传入run_live的配置可用。

**本步停止点：找到调用，认出三个参数，就跳到第2步。**

自检：main是否亲自读取一帧视频？

答案：没有。它准备配置、设置信号处理、调用run_live，并在外层捕获异常。

## 第2步：认识总调度函数，先不钻进细节

打开[src/manager/live_pipeline.cpp](../src/manager/live_pipeline.cpp)，搜索`int run_live`。
不要从文件前面的`StatsFile`开始读，它只是写报告的辅助对象。

先在run_live里圈出这些对象：

| 对象 | 暂时这样理解 |
|---|---|
| `group.videos` | 多路视频输入，每路有自己的读取线程 |
| `group.audios` | 多路音频输入，每路也有自己的读取线程 |
| `video_mixer` | 把多张画面拼成节目画面 |
| `audio_mixer` | 把同一时刻的各路声音相加 |
| `video_encoder` / `audio_encoder` | 把节目原始数据压缩成H.264/AAC |
| `output` | 接收编码包，由另一线程写文件或推流 |
| `recorder` | 取得编码包的一份引用，独立录制 |

继续搜索`while (!stop)`，找到节目主循环；再找到`if (video_turn)`及对应的音频`else`分支。
第一遍只给代码划出四块：**建立对象 → 启动输入 → 循环制作节目 → 收尾退出**。

注意：run_live在调用它的主线程运行。输入和输出对象内部启动线程，不代表每一个函数调用都切换线程。

**本步停止点：认出视频分支和音频分支，先不研究video_turn怎么算。**

自检：所有文件的功能在哪里被串起来？

答案：就在run_live，它是你读完其他模块后不断返回的地方。建议一直保留这个编辑器标签页。

## 第3步：花几分钟区分Packet和Frame

暂时打开[include/common/ffmpeg.h](../include/common/ffmpeg.h)，只看`PacketDeleter`、`Packet`、`make_packet`。
再打开[include/codec/media_codec.h](../include/codec/media_codec.h)，只看`FrameDeleter`、`Frame`、`make_frame`、`FrameConsumer`。

你现在需要记住：

```text
输入的压缩数据 Packet → 解码 → 原始数据 Frame
节目原始数据 Frame   → 编码 → 输出的压缩数据 Packet
```

视频Frame装像素，音频Frame装PCM采样。Packet里是编码后的数据，不能直接拿去拼图或做采样求和。

`Packet`/`Frame`是项目的智能指针类型，分别拥有一个FFmpeg对象外壳。读代码时认三个操作：

| 操作 | 在本项目中意味着什么 |
|---|---|
| `frame.get()` | 临时借出指针，所有者仍是frame |
| `std::move(frame)` | 把所有权交给接收方，原变量不再持有该对象 |
| `av_frame_ref` / `av_packet_clone` | 建立另一份引用；外壳独立，底层缓冲可以共享 |

`make_frame()`只分配外壳，不自动生成一张有像素的图片。自行生成图像还需`av_frame_get_buffer`。
解码器接收函数则会给帧填入解码结果的引用。

**本步停止点：能区分原始数据与压缩数据，回到视频输入路线。IoControl先略过。**

自检：一个Frame移进队列以后，生产者还能继续拿原来的frame改图吗？

答案：不能。所有权已交给队列；生产者不应再使用旧指针访问已交付的对象。

## 第4步：第一包压缩视频从哪里来

回到run_live，搜索`new VideoSource`。这里为每个视频输入创建一个对象。
跳到[src/input/video_source.cpp](../src/input/video_source.cpp)，依次找：

1. `VideoSource::VideoSource`：找到`std::thread`，确认它启动`VideoSource::run`。
2. `VideoSource::run`：找到`StreamInput input(io)`和`input.open(url, log)`。
3. 函数靠后的读取循环：找到`input.read(packet.get())`。
4. 找到`packet->stream_index == index`，然后是`decoder.send(packet.get(), consume)`。

先略过信号掩码、时间戳修复和consume回调正文。当前只追问：input.read怎么得到包？

打开[src/input/stream_input.cpp](../src/input/stream_input.cpp)，读`StreamInput::open`和`StreamInput::read`：

```text
avformat_open_input       打开文件或网络输入
avformat_find_stream_info 探测有哪些轨道及它们的参数
av_read_frame            取出下一包压缩数据
```

名字里的`read_frame`容易误导：这个API返回的是AVPacket，不是已经解码好的AVFrame。
同一个文件可能交错出现视频包、音频包，所以视频线程通过stream_index只把视频包送给视频解码器。

**本步停止点：回到VideoSource的decoder.send调用，不继续深挖网络协议。**

自检：StreamInput负责把H.264解码成像素吗？

答案：不负责。它解复用，只负责从容器/网络流中取出压缩包。

## 第5步：压缩包怎样变成画面

从`decoder.send`跳到[src/codec/media_codec.cpp](../src/codec/media_codec.cpp)。
本轮只按这个顺序看解码部分：

1. `MediaDecoder::open`：从输入轨道复制编码参数并打开解码器。
2. `MediaDecoder::send`：调用`avcodec_send_packet`提交压缩包。
3. `MediaDecoder::drain`：循环调用`avcodec_receive_frame`拿原始帧。
4. `consume(std::move(frame))`：把得到的帧交回调用方。

一次送包不保证立刻产出一帧，也不保证只产出一帧。因此不能把send/receive写成固定“一包换一帧”。
这里的EAGAIN表示当前操作需要另一侧先推进；EOF表示排空已结束。

现在返回[src/input/video_source.cpp](../src/input/video_source.cpp)，找到：

```cpp
const FrameConsumer consume = [&](Frame frame) {
```

这就是刚才drain调用的consume。它虽然写在读取循环前面，但定义时不会处理帧；解码器实际调用它时才执行正文。
**回调仍在这一路视频输入线程里执行，没有新建线程。**

**本步停止点：沿着consume正文找到converter.convert，接第6步。**

自检：解码器文件怎么知道下一步要缩放、入队？

答案：它不知道具体业务；VideoSource把后续处理放进consume，解码器只负责把帧交给这个回调。

## 第6步：让各路画面变成统一格式，再交给主线程

继续读consume，这次关注三件事：

1. 源PTS换成微秒，减容器共同起点，再加配置偏移。
2. `converter.convert`把尺寸和像素格式统一。
3. `queue_.push(std::move(converted))`把帧移交给队列。

暂时把SourceClock当成“修补异常时间戳的工具”；等主线走通再看它的算法。

打开[src/codec/video_converter.cpp](../src/codec/video_converter.cpp)，读`VideoConverter::convert`的两个分支：

- 已经是目标尺寸的YUV420P：创建新的帧引用，共享像素。
- 其他情况：分配目标图像，使用`sws_scale`缩放并转换。

然后打开[include/queue/frame_queue.h](../include/queue/frame_queue.h)。这个模块实现就在头文件，没有对应cpp。
先读`push`，再读`advance`。push从输入线程接收帧，advance由节目主线程按时间选帧。

例如队列PTS是0、40000、80000微秒，节目需要60000微秒的画面：advance取40000，保留80000供未来使用。
被选中的帧放进调用方的current，所以离开队列后仍然有人持有它。

**本步停止点：回到run_live的视频分支，找q.advance(us, current[i])。**

自检：两个线程之间真正的数据交接发生在哪里？

答案：FrameQueue。输入线程push拥有的Frame，主线程advance取得它；不是让两个线程同时操作同一个解码器。

## 第7步：从多路画面得到一张节目画面

在[src/manager/live_pipeline.cpp](../src/manager/live_pipeline.cpp)的视频分支里依次找：

```text
q.advance → frames.push_back → video_mixer.mix
```

current保管每路上一张有效画面；frames只是本轮借给合成器的指针列表。
没有可用画面或旧画面超过stale_ms时，放入黑帧。短时间没有新帧则允许沿用上一张。

打开[src/mixer/video_mixer.cpp](../src/mixer/video_mixer.cpp)：

1. 先看`configure`里的`layout == "split"`分支：两路缩小到半宽，然后hstack左右拼接。
2. 再看`mix`：每路建立帧引用，送进buffer节点，再从buffersink取节目帧。

第一次不需要背完整滤镜字符串，只记住：

```text
两路原始图像 → scale缩放各子画面 → hstack拼接 → 一张原始节目图像
```

输入滤镜图的PTS用tick，也就是节目帧序号。25fps时tick=1表示40ms。
从滤镜拿到的画面尚未压缩，接下来还要编码。

**本步停止点：回run_live，找到video_encoder.send。**

自检：合成器返回的是H.264吗？

答案：不是。它返回原始AVFrame，H.264由下一步的编码器产生。

## 第8步：把节目画面压缩成H.264

在run_live中先看`frame->pts = av_rescale_q(...)`，再看`video_encoder.send(frame.get(), video_packet)`。

这里必须换时间单位：25fps的tick=1是40ms，编码器time_base为1/90000，因此PTS应是3600。
画面还是同一时刻，只是从“帧序号”换成“九万分之一秒的刻度”。

返回[src/codec/media_codec.cpp](../src/codec/media_codec.cpp)，这次读编码部分：

```text
MediaEncoder::open_video → send → drain → consume(std::move(packet))
```

open_video建立libx264编码器；send提交原始帧，drain取得压缩包。码率/VBV的调优先略过。
最后的consume对应调用方传入的video_packet。

回run_live搜索`const PacketConsumer video_packet`：它设置视频流序号0，先调用recorder.submit，再调用output.submit。
音频回调使用流序号1。这里的0/1是节目输出的约定，和原输入文件里的轨道序号不是同一个概念。

本轮先沿output走。只记住recorder先克隆自己的包引用，原包再move给output，两个支路不会争抢同一个包外壳。

**本步停止点：从output.submit进入第9步。**

自检：编码器的回调在哪个线程执行？

答案：此处就在节目主线程。编码器内部可能有FFmpeg线程，但本项目的消费回调在send/drain调用栈中同步执行。

## 第9步：编码包怎样真正写进文件

打开[src/output/async_output.cpp](../src/output/async_output.cpp)，只沿本地输出的正常路径读：

1. `AsyncOutput::open`复制编码参数并启动输出线程。
2. `submit`把Packet移进PacketQueue。
3. `run`中的`queue_.pop(item)`取到包。
4. `sink->open`建立输出，`sink->write`写包。

文件开头的`FfmpegSink`把这些调用转给StreamOutput。recover、epoch、重试退避暂时略过。
输出线程与节目线程通过队列连接，所以submit完成不等于文件已经写完。

打开[src/output/stream_output.cpp](../src/output/stream_output.cpp)，按顺序看：

| 方法 | 关注的FFmpeg操作 | 作用 |
|---|---|---|
| open | 创建输出上下文、轨道、AVIO，avformat_write_header | 建立FLV容器并写头 |
| write | av_packet_rescale_ts、av_interleaved_write_frame | 换成容器时间基并交错写入音视频包 |
| finish | av_write_trailer、avio_flush | 排出缓存并完成容器尾 |

open复制编码参数，不会重新编码。容器可能在写头时调整time_base，所以write必须读取写头后的值。
比如编码器PTS=3600（1/90000）换到FLV的1/1000刻度后是40，仍是40ms。

**本步停止点：你已走通视频主线，可以停下来，下一次再读音频。**

请不看代码复述一次：

```text
VideoSource线程：StreamInput → MediaDecoder → VideoConverter → FrameQueue
节目主线程：    FrameQueue → VideoMixer → MediaEncoder → PacketQueue
输出线程：      PacketQueue → StreamOutput → 本地FLV
```

如果只能记住一件事，就记这三行以及它们之间的两次队列交接。

## 第10步：接上音频，不重新学习已经看过的部分

打开[src/input/audio_source.cpp](../src/input/audio_source.cpp)，读`AudioSource::run`。
StreamInput和MediaDecoder已经认识了，直接找到`const FrameConsumer decoded`。

它把解码后的音频交给resampler，再将输出帧放进AudioQueue。同一个URL的音视频在当前实现中由不同线程分别打开。

依次打开：

1. [src/sync/audio_resampler.cpp](../src/sync/audio_resampler.cpp)：先看首次创建SwrContext、swr_convert及输出回调。第一遍略过漂移补偿分支。
2. [include/queue/audio_queue.h](../include/queue/audio_queue.h)：读read_window，理解按采样区间取数据。
3. 回run_live的音频else分支：看audio_frame预填静音，再由read_window覆盖已有声音。
4. [src/mixer/audio_mixer.cpp](../src/mixer/audio_mixer.cpp)：读mix的采样循环，理解逐路增益后相加。
5. 回run_live：audio_encoder.send将混音帧编码成AAC，再通过audio_packet进入同一个异步输出。

目标音频统一为48kHz、双声道、FLTP。PTS不是毫秒，而是每声道的采样位置。
一个1024采样的块约21.333ms；双声道不把这段时间再乘二。

目标窗口为[0,1024)，如果源只提供[240,720)，队列就只复制中间480个采样，其余保留静音。
音频不能像视频那样一直重复上一块，否则会反复播放旧声音。

**本步停止点：确认音视频最后都成为Packet，汇入同一个output。**

自检：为什么混音前要先对齐窗口？

答案：采样求和必须发生在同一节目时刻，不能把A路第1秒和B路第2秒的声音直接相加。

## 第11步：现在才细读调度器如何决定先做视频还是音频

回[src/manager/live_pipeline.cpp](../src/manager/live_pipeline.cpp)，搜索`av_compare_ts`。
这里比较视频下一帧时刻与音频下一块时刻，哪个更早就先制作哪个；同一时刻先视频。

用25fps、48kHz和1024采样块手算前几轮，暂不考虑末尾短块和实时跳帧：

| 循环前tick | 循环前cursor | 视频时刻 | 音频时刻 | 本轮处理 |
|---|---|---|---|---|
| 0 | 0 | 0ms | 0ms | 视频；tick变1 |
| 1 | 0 | 40ms | 0ms | 音频；cursor变1024 |
| 1 | 1024 | 40ms | 21.333ms | 音频；cursor变2048 |
| 1 | 2048 | 40ms | 42.667ms | 视频；tick变2 |

因此代码不能比较`tick < cursor`。单位不同，整数大小没有直接意义。

再看`if (options.realtime)`：它让节目时刻跟单调墙钟的经过时间匹配。源PTS用来选择内容，墙钟用来控制制作节奏。
最后再回看视频源减共同原点、音频源换算采样位置的代码，理解它们为什么要先统一时间轴。

**本步停止点：能够手算上表，就暂时不看复杂漂移和追帧策略。**

自检：把每路音视频分别减去自己的首帧时间，会有什么问题？

答案：可能抹掉原有轨道偏移。例如声音原本比画面早100ms，分别归零后这个关系就丢了，所以要尽可能使用容器共同起点。

## 第12步：程序什么时候停，资源由谁释放

回run_live搜索`video_encoder.send(nullptr`，从这里读到函数结束，再回看`Sources::shutdown`。

正常结束的主要顺序是：

```text
停止制作新节目帧 → 排空视频/音频编码器 → 排空异步输出并写尾
→ 完成录制支路 → 停止并join输入线程 → 汇总统计
```

取消路径不同：收到停止请求后优先cancel输出，不保证发送完所有缓存尾包。

然后分别去VideoSource和AudioSource看stop/join：

- stop设置原子取消标志，让阻塞I/O有机会中断。
- 同时close队列，唤醒可能卡在满队列push上的线程。
- join等待线程真的结束后，才能安全销毁它还会访问的对象。

最后回[include/common/ffmpeg.h](../include/common/ffmpeg.h)读IoControl，补上“FFmpeg读写怎样发现取消和超时”。
Frame/Packet等局部智能指针离开作用域时释放其持有的引用；写尾则是需要显式执行并检查错误的业务动作。

**本步停止点：到这里，启动、媒体主线和退出都已闭合。**

自检：只调用join，不发stop也不关闭满队列，为什么可能一直等？

答案：工作线程可能永远在等待队列空位，而主线程已经不再消费；join只等待，不会替你解除阻塞。

## 第13步：用小测试检验自己是否真懂了

不要一开始就单步调试整个多线程节目。先打开这三个短文件：

| 顺序 | 文件 | 先预测什么 |
|---|---|---|
| 1 | [tests/video_reference_test.cpp](../tests/video_reference_test.cpp) | shared改成99后，source是否仍为77？source释放后surviving是否还能读取？ |
| 2 | [tests/frame_queue_test.cpp](../tests/frame_queue_test.cpp) | 容量2已有两帧时，第三次push何时返回？close会唤醒谁？ |
| 3 | [tests/audio_sync_test.cpp](../tests/audio_sync_test.cpp) | 增益稳定后0.25×0.5是多少？窗口外采样为什么仍为0？ |

答案分别是：写时复制保持source为77，surviving有引用仍可读；消费腾空或close解除等待；幅度为0.125，窗口外保留预填静音。
音频测试后面的十分钟模拟漂移部分可以留到下一步。

可在项目根目录运行现有测试（需要已安装项目构建依赖）：

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j 4
ctest --test-dir build/debug --output-on-failure -R '^(video_reference|frame_queue|audio_sync)$'
```

这是阅读练习的复现命令；本文新增没有修改代码，也未把历史测试成绩当成本次执行结果。

## 第14步：主线熟悉后，每次再补一个专题

下面按顺序继续，不要同时打开所有文件：

| 专题 | 从哪里进入 | 本轮要回答的问题 |
|---|---|---|
| 输入时间异常与音频漂移 | av_synchronizer.h的SourceClock::map → audio_resampler.cpp的error分支 → audio_sync_test.cpp | 缺失PTS如何推算？小漂移为什么渐变补偿，大间断为什么保留静音？ |
| 网络背压与输出恢复 | packet_queue.h的push/reset_locked → async_output.cpp的recover与run → output_recovery_fixture.cpp | 丢过期包后为什么等新IDR？握手后为什么再检查epoch？ |
| 分段录制 | 回run_live的recorder.submit → recorder.cpp的submit/run/finalize | 两个支路怎样各自拥有包？为何等关键帧切段、写尾成功才发布索引？ |
| 独立回放 | replay_main.cpp的main | 如何按索引找段、向前找关键帧、重设输出原点？为什么不是精确逐帧剪辑？ |
| 配置 | live_main.cpp的main → live_config.cpp的load_live_config | JSON如何变成选项？命令行如何覆盖？哪些检查发生在打开媒体之前？ |
| 性能与日志 | live_pipeline.cpp的report_metrics → metrics.cpp；再看logger.cpp | 计数的单位是什么？CPU、API耗时、调度差能否代表远端播放延迟？ |
| 环境诊断 | env_check_main.cpp的main | 检查版本和能力注册为什么不等于真实推流成功？ |

这一轮的输入异常处理与输出重连要分清：当前VideoSource/AudioSource失败后关闭该路队列并降级，没有在输入端实现自动重连。
恢复夹具使用可控传输边界，不能代替真实RTMP/RTSP成功会话验收。

## 今天只读到哪里就够了

第一次只完成第1～3步，然后亲手在编辑器里找出下列三个位置：

1. main调用run_live的位置。
2. run_live里`if (video_turn)`的视频分支。
3. Frame和Packet各自的类型定义。

能用自己的话说清“main交配置，run_live组织节目，Frame是原始数据，Packet是压缩数据”，就可以结束第一次阅读。
下次直接从第4步继续。每读完一段回到run_live找它的调用位置，你会逐渐把散开的文件连成完整流程。
