# 工程与媒体测试记录

> 当前源码已于2026-11-09收敛为完整直播、独立回放和环境诊断三个入口。
> 本文保留有日期的开发历史；旧阶段代码/命令不再作为当前入口。
> 当前使用与结构见[README](../README.md)、[架构](architecture.md)和[阅读路线](code-reading-guide.md)。


## Phase 0 历史记录

日期：2026-10-29（Asia/Shanghai）。
本文件目前记录工程验收，不代表媒体性能测试。

## 环境

- Ubuntu 24.04.4 LTS，WSL2，x86_64。
- CPU：Intel Core i7-14650HX，WSL 可见 24 个逻辑 CPU。
- WSL 内存约 15 GiB，交换空间 4 GiB。
- GCC/G++ 13.3.0，CMake 3.28.3，pkg-config 1.8.1，GDB 15.1。
- FFmpeg 6.1.1-3ubuntu5。
- libavformat 60.16.100，libavcodec 60.31.102，libavutil 58.29.100。
- libavfilter 9.12.100，libswresample 4.12.100，libswscale 7.5.100。

## 真实结果

| 项目 | 结果 |
|---|---|
| Debug/Release CMake 配置 | 均成功找到六库及 Threads |
| Debug/Release 编译 | 均成功，无编译警告 |
| 编译标准 | 两种配置均 -std=c++11，禁用扩展 |
| 环境程序 | 两种配置均退出 0，头文件和运行库版本完全一致 |
| 能力查询 | libx264、AAC、FLV、RTMP 输出协议均已注册 |
| Debug CTest | 2/2 通过，总耗时约 0.02 秒 |
| Release CTest | 2/2 通过，总耗时约 0.02 秒 |
| GDB | main 源码断点命中，继续执行正常退出 |
| VSCode F5 界面 | 未实测，待人工验收 |

日志测试验证 4 线程 400 条日志恰好各出现一次、不交错，并检查级别过滤、
换行转义。CTest 耗时仅用于复核测试执行，不作为吞吐性能指标。
复现命令见 README.md。

未执行媒体输入、编解码、合成、混音、RTMP、压力测试或长时间稳定性测试。
媒体路数、分辨率、帧率、编码配置、CPU 占用、丢帧率和延迟均不适用，
没有对后续阶段性能指标作结论。

## Phase 1 验收记录

日期：2026-10-29，Asia/Shanghai。操作系统、CPU、内存及 FFmpeg 与 Phase 0 相同，
测试解释器为 Python 3.12.3。以下是短样片功能/异常验收，不是直播吞吐性能报告。

素材：单路 320×180 / 25fps / 3 秒 testsrc2 视频，libx264 fast、GOP=25、
B 帧=2、YUV420P；单路 48kHz 正弦音频，AAC，编码器默认码率。
每份主样片共 217 个包。未测 CPU 占用、端到端延迟或长期内存趋势。
核心 Remux 由 C++ FFmpeg API 完成；FFmpeg CLI 仅生成/检查样片。

最终修改后的构建与验证：

| 配置 | 编译 | CTest | 总耗时 | 3 秒样片实时发送耗时 |
|---|---|---|---|---|
| Debug | 成功，无警告 | 3/3 通过 | 6.09 秒 | 3.084 秒 |
| Release | 成功，无警告 | 3/3 通过 | 6.31 秒 | 3.088 秒 |
| ASan + UBSan | 成功，无警告 | 3/3 通过 | 6.91 秒 | 3.109 秒 |

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1，
UBSAN_OPTIONS=halt_on_error=1；覆盖路径未报告泄漏、地址错误或未定义行为。
系统 FFmpeg 库并未重新插桩，不能将结果推广为所有库内部行为无问题。
构建均保留 -std=c++11，Debug 使用 -g，Release 使用 -O3/-DNDEBUG。

三个 CTest 分别为环境检测、并发日志及媒体集成测试。
媒体集成测试内部覆盖以下场景：

| 场景 | 真实结果 |
|---|---|
| 含 B 帧 MP4 → FLV | 成功，217 包数量和 SHA-256 数据哈希一致 |
| 时间基换算 | 视频 1/12800、音频 1/48000 → FLV 1/1000 |
| PTS-DTS 和音视频相对原点 | 保持，逐包比较误差小于 2ms |
| 正常输出解码 | ffmpeg -xerror 解码到 null 成功 |
| 默认实时节奏 | 约 3.09 秒，无一次性突发发送 |
| 流顺序交换 + 300ms 音频延迟 | 保持映射与共同时间平移 |
| 仅视频/仅音频 | Remux、包校验和解码通过 |
| FLV 标签注入重复 DTS | 非零退出，明确时间戳错误 |
| 已有文件、输入同路径、符号链接 | 拒绝输出，原文件哈希不变 |
| 文件缺失、损坏、不兼容 MPEG-4 编码 | 非零退出，不兼容编码在创建输出前拒绝 |
| 裸 H.264 缺失 PTS/DTS | 明确拒绝，不伪造时间戳 |
| 无效超时、输出目录不存在 | 非零退出 |
| RTMP/RTSP 输入连接拒绝 | 非零退出 |
| RTMP 输出连接拒绝 | 非零退出 |
| RTSP/RTMP 输入 TCP 接通但不响应握手 | 300ms 配置触发超时，测试上限 3 秒内退出 |
| RTSP 输入阻塞时 SIGINT | 退出 130，无挂起 |
| 实时发送中 SIGINT | 退出 130，部分 FLV 可读取并解码 |
| RLIMIT_FSIZE 限制输出为 4096 字节 | 真实写入失败被报告，退出 1 |

TCP 测试只模拟故障，不实现或部署 RTMP/RTSP 媒体服务端。
短暂的可解码部分输出不代表异常时总能保留完整节目。
第一次测试因素材命令使用 .3 而失败，定位为 FFmpeg duration 参数格式，
修正为 0.3 后重跑完整测试；没有跳过该场景。

手工运行 scripts/generate-sample.sh 成功，build/samples/output.flv 解码检查通过。
GDB 在 StreamOutput::write 命中断点，观察 input_time_base=1/12800、
origin_us=-80000，继续后写入 217 包并正常退出。
VSCode JSON 配置可解析；图形界面 F5 仍未实测。

复现：见 README 的构建、CTest、内存检查和样片命令。
原始最近测试输出位于各构建目录 Testing/Temporary/LastTest.log（构建目录不入 Git）。

仍未验收：

- 实际 RTSP/RTMP 成功输入会话、运行中网络断流。
- 实际 RTMP 服务端接收和播放器播放，输出连接建立后的阻塞/断流。
- 人工听看、长时间运行、端到端延迟、采样级首尾裁剪。
- Phase 2 及以后全部功能。

当前停留在 Phase 1，不能宣称真实直播链路已全部验收。

## Phase 2 验收记录（2026-10-29）

Debug/Release 编译通过，CTest 各 4/4（8.12/8.14 秒）；
ASan+UBSan 含泄漏检查 4/4（9.00 秒），覆盖路径未报告错误。
新增 transcode_integration：53 帧 H.264 B 帧、27 帧 H.265 10-bit，
输出帧数完整、YUV420P/640×360/H.264，44.1kHz 单声道输入转换为
48kHz 双声道 AAC，300ms 音频偏移在编码延迟容差内保留。
验证了逐流 DTS 单调、首尾音画时间差、实际音频采样时长、完整输出解码、
单音轨、旧文件保护、错误参数和 SIGINT 收尾。
手工样片 build/samples/transcoded.flv 含 75 视频帧，可完整解码。
成功网络会话、人工视听和长期性能仍未验收；没有部署服务端。

## Phase 3 验收记录（2026-10-29）

硬件/系统与前述相同。按顺序先完成 Phase 2，之后才加入多输入线程和合成。

| 配置 | 结果 | 耗时 |
|---|---|---|
| Debug 全套 | 6/6 通过 | 11.39 秒 |
| Debug 补充动态调度用例后，队列+合成复测 | 2/2 通过 | 5.24 秒 |
| Release 最终全套 | 6/6 通过 | 12.02 秒 |
| ASan+UBSan 最终全套（含泄漏检查） | 6/6 通过，未报告错误 | 13.15 秒 |

所有配置编译成功，无编译警告。Debug 后续只修改了合成源容器容量预留，
并增加动态画面测试，因此针对队列/合成复测；Release/ASan 执行最终完整六项。

输入素材：四路 H.264、160×120/15fps、256×144/30fps、320×180/24fps、
128×96/25fps，分别为红、绿、蓝、黄；每路 2 秒，含 B 帧。
输出：H.264/YUV420P，320×240/20fps，名义编码目标 1.2Mbps，GOP=40，B 帧=0。
纯色素材实际码率远低于目标，不能用其评估真实赛事编码压力。

| 场景 | 验证结果 |
|---|---|
| 四路合成 | 输出 40 帧，四宫格各区域像素颜色正确 |
| 双画面/右下画中画 | 实际像素区域正确，可完整解码 |
| 1 秒处 split→pip | 切换前后像素变化正确，PTS 连续 |
| 各种输入帧率 | 输出包 PTS 间隔 50ms，误差小于 2ms |
| 起点 5 秒且动态变色的 15fps 输入 | 起点归零，正确时段从红变白，输出 20fps |
| 短源先结束 | 暂留最后画面，stale_ms 后变黑，另一源持续输出 |
| 一路输入文件不存在 | 明确警告，仍输出正常源和黑色区域 |
| 全部输入失败 | 非零退出，不伪造正常节目 |
| 实时输出 1 秒 | Debug 0.998s / Release 0.984s / ASan 1.025s（20 帧） |
| 队列上限 | 所有报告峰值 ≤8 帧，满队列产生背压 |
| 队列关闭 | 生产者/消费者等待都能被唤醒 |
| SIGINT | 四路并发及背压下全部线程退出，部分 FLV 可解码 |
| 已有输出、非法布局/尺寸 | 拒绝执行，已有文件不变 |

20 帧最后一帧时刻为 0.95 秒，运行时间接近 1 秒，不要求恰好等到 1.000 秒。
这些是调度和功能测试时间，不是端到端直播延迟。

演示产物（build 不入 Git）：

- build/samples/transcoded.flv：Phase 2，75 视频帧和 AAC。
- build/samples/split.flv：双画面，75 视频帧。
- build/samples/grid.flv：四宫格，640×360/25fps，75 视频帧。
- build/samples/grid-preview.png：从实际四宫格视频提取的首帧，已查看。

Phase 3 不输出音频；单路转码可输出音视频，多路混音需要 Phase 4。
尚未测试成功 RTMP 播放、多路网络源、网络中途断流、1080p 多路性能、长期运行，
没有测量 CPU/内存长期趋势或声明性能目标达成。当前保持这些验收项未完成。

## Phase 4 验收记录（2026-10-29）

环境与 Phase 3 相同：Ubuntu 24.04 / WSL2、GCC 13.3、FFmpeg 6.1.1。
没有新增系统依赖或部署服务端。新增 audio_sync、live_integration，现有六项测试保留。

| 配置 | 全套结果 | 全套耗时 | 最终同步用例复测 |
|---|---|---|---|
| Debug | 8/8 通过 | 17.24 秒 | 1/1，6.31 秒 |
| Release | 8/8 通过 | 17.31 秒 | 1/1，5.89 秒 |
| ASan+UBSan | 8/8 通过 | 19.70 秒 | 1/1，6.68 秒 |

编译均成功、无警告。ASan 开启 detect_leaks，UBSan 开启 halt_on_error，覆盖路径未报告错误。
最后仅修正内容测量方法、收紧容差和完善“启动无媒体即失败”的检查，
因此针对 live_integration 在三个配置下重新执行；结果如最后一列。

素材/参数：

- 两路视频 160×120 / 25fps，合成 320×240 / 25fps、H.264、名义1.2Mbps。
- 游戏声为 440Hz、44.1kHz 单声道 AAC；解说声为 880Hz、48kHz 单声道 Opus。
- 音频输出 AAC、48kHz 双声道、128kbps；内部 PCM 为 FLTP。
- 内容同步素材：视频在1秒闪白，音频脉冲因容器设置晚250ms；另测提前250ms配置。

| 项目 | 实际结果 |
|---|---|
| AAC/Opus 混音 | 输出解码后 440Hz 幅度0.1248、880Hz 幅度0.1249，两路都存在 |
| 增益0.5 | 目标频率幅度比在0.44–0.56，另一频率保持在0.9–1.1 |
| 单路静音 | 对应880Hz幅度低于0.003，440Hz仍高于0.04 |
| 运行中音量变化 | 1秒处把指定输入渐变至0，变化前后频谱验证通过 |
| 正常音量失真检查 | 输出峰值小于1，1320/1760Hz额外谐波低于0.006 |
| 过载保护单测 | 编码前 PCM 峰值不超过0.95001，限峰计数非零 |
| 音频采样窗口 | 带240采样偏移的480采样块准确落在请求区间，其余位置为零 |
| 源缺失/无音轨 | 记录降级静音，其他音频继续；全部启动无媒体不创建节目 |
| 内容偏移保留 | 250ms配置实际测为249ms |
| 内容对齐 | 提前250ms后实际测为−1ms |
| 30秒真实编码输出 | 31条CSV记录，记录点调度差峰值40000us，末端差不累积 |
| 600秒、−1000ppm模拟 | 最终相位误差48采样，峰值49采样，28101次软补偿 |
| 600秒、+1000ppm模拟 | 最终相位误差46采样，峰值48采样，28101次软补偿 |
| 时间戳异常 | 缺失、重复、回退、300ms缺口、30秒跳变均有对应处理与计数 |
| 队列背压/退出 | 音频队列不超过32块，关闭唤醒，SIGINT部分产物可解码 |
| 输出保护 | 既有媒体/CSV不覆盖，非法增益和索引被拒绝 |

音频内容事件用10ms RMS窗口检测，视频按实际帧 PTS；测得−1ms并不意味着测量精度为1ms。
源相位误差、调度差和内容同步差是不同指标。25fps的40ms调度差不能作为口型偏差。
600秒模拟在短时间内执行，只是媒体时间跨度；没有实际运行10分钟、24小时或真实声卡时钟实验。
30秒测试是真实生成并编码了30秒媒体，使用离线加速，也不是30秒墙钟直播。

定位并修正的测量问题：默认 FFmpeg 原始帧导出会补帧，使按下标计算的内容差偏移40ms。
改为 -fps_mode passthrough，匹配实际包 PTS 并校验帧数，容差从50ms收紧至20ms后通过。
未通过放宽标准或改变被测输出掩盖问题。

保留演示：build/samples/phase4-demo.flv，包含75帧视频、144000个编码前音频采样；
build/samples/phase4-sync.csv 包含起点、每秒和结束记录。样片通过完整解码检查。
独立声源由 scripts/generate-audio-samples.sh 生成；复现命令见 README 的 Phase 4 部分。

未验收：真实 RTMP 播放、实际设备多源采集、真实长时间运行、人工听感、
异常边界交叉淡化、动态音频格式切换、NTP/设备时钟关系自动推断。
源网络 URL 被音视频分别打开的起点差异需要实际验证，不能由文件测试推断网络同步通过。
Phase 4 结束时尚未实现 Phase 5；后续进度见下方 Phase 5 记录。


## Phase 5 验收记录（2026-10-29）

环境沿用 Ubuntu 24.04/WSL2、GCC 13.3、CMake 3.28.3、FFmpeg 6.1.1；
未安装新依赖，未部署 RTMP 服务端。Debug/Release/ASan 配置与编译全部成功，
构建日志未出现编译警告；Debug/Release 编译参数确认包含 -std=c++11。

| 配置 | 完整 CTest | 完整耗时 | 最终日志指标修改后相关复测 |
|---|---|---|---|
| Debug | 10/10 通过 | 23.78秒 | live/output integration 2/2，8.43秒 |
| Release | 10/10 通过 | 22.38秒 | 2/2，7.99秒 |
| ASan+UBSan | 10/10 通过 | 25.43秒 | 2/2，8.86秒 |

最后仅增加结束日志的音视频丢包率，三个配置均重新编译并复测相关两个集成测试。
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1、UBSAN_OPTIONS=halt_on_error=1，
覆盖路径未报告地址、泄漏或未定义行为错误；系统 FFmpeg 未重新插桩。
不同配置并行运行，耗时不能作配置间性能比较。

新增 packet_queue 和 output_integration，辅助程序 output_recovery_fixture 使用真实编码器与封装器。

| 场景 | 结果 |
|---|---|
| 包数/字节/时间跨度上限 | 均触发预期清空，峰值未超过配置 |
| IDR门控/epoch失效 | 旧窗口不继续发送，等待新关键帧 |
| 本地4包背压 | 75视频帧完整，零丢包，排空后队列为零 |
| 首次打开延迟120ms、预算80ms | 保留尚未发送媒体的新连接，改等新IDR |
| 注入一次写失败（同时清空原包引用） | 不重发失败包，重新写头恢复 |
| 注入一次100ms慢写 | 丢弃积压窗口，恢复计入总重试预算 |
| 恢复产物 | 3个非空真实FLV均首视频包为关键帧、DTS=0、逐流DTS单调、完整解码通过 |
| RTMP连接拒绝 | 使用实际FFmpeg网络路径，重试上限2及禁用0均符合配置 |
| RTMP握手不响应 | 实际FFmpeg在200ms超时设置下失败，随后第二次TCP连接被观测到 |
| 退避取消/EOF等待中SIGINT | 及时退出，无线程挂起；EOF未恢复报错 |
| 参数/输出保护 | 非法队列与编码参数被拒绝，RTMP禁止离线突发模式，已有文件不覆盖 |

Debug 最后一次注入恢复记录：3会话、2次重试，成功视频195包/音频361包，
丢视频55包/音频109包，队列峰值6包/814字节，成功写入前排队峰值5169us，
写入峰值100135us。线程时序可能使包数变化，因此测试检验容量/可解码性等不变量，
不要求故障期间每次丢包数量一致。此传输为测试替换边界，不是成功RTMP会话。
TCP无响应测试仅注入故障，不提供RTMP媒体服务。

### 本地实时节奏样片

输入两路纯色视频及440Hz AAC/880Hz Opus，输出320×240/25fps、H.264目标1.2Mbps，
GOP1秒、VBV500ms、AAC 48kHz双声道。duration=3000ms，输出队列上限4包；
没有 --no-realtime，实际按墙钟调度约2.99秒，最后事件不必等到3.000秒。

- 产物：build/samples/phase5-local.flv、phase5-local.csv。
- ffprobe：75视频帧、142 AAC帧；ffmpeg -xerror 完整解码退出0。
- 平均25.0905fps，编码数据141.294kbps，音视频丢包均0，恢复0次。
- 队列峰值1包/886字节，排队峰值175us，写入峰值64us，结束队列为0。
- 编码前媒体末端视频/音频均3秒；AAC容器长度受编码器预卷和填充影响。

纯色画面码率低，不能代表赛事复杂画面的编码压力；本地写入延迟不能代表网络延迟。
没有测量真实端到端延迟、CPU性能瓶颈、持续内存趋势或24小时直播稳定性。
实际RTMP成功连接、播放、服务端重启/网络中断后的恢复、真实内容同步仍未验收。

原始构建/测试日志位于 build/phase5-{debug,release,asan}.log、
对应的 -final.log 和 build/phase5-demo.log；这些生成文件不入Git。
复现命令及全部新参数见 README 的 Phase 5 部分。Phase 6 未实施，没有 Git 提交。


## Phase 6 验收记录（2026-10-29）

环境沿用Ubuntu 24.04/WSL2、GCC13.3、CMake3.28.3、FFmpeg6.1.1。
CMake构建新增Recorder及esports_replay，仍为C++11，无新增系统依赖或服务端。
Debug/Release/ASan配置与编译均成功，编译日志未出现warning/error。

| 配置 | 完整CTest | 总耗时 | 1秒回放墙钟 |
|---|---|---|---|
| Debug | 11/11通过 | 28.72秒 | 1.014秒 |
| Release | 11/11通过 | 27.28秒 | 1.005秒 |
| ASan+UBSan | 11/11通过 | 31.27秒 | 1.036秒 |

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1、UBSAN_OPTIONS=halt_on_error=1，
覆盖路径无报告错误。系统FFmpeg未重新插桩；不是全程序长期内存稳定性证明。
三种配置同时运行，耗时不能用作性能优劣比较。

新增recording_integration真实生成3.2秒160×120/25fps动态视频和440Hz音频，
合成320×240/25fps、H.264目标1.2Mbps、GOP1秒、AAC48kHz双声道。
直播FLV与录制文件从同一编码回调分流，测试没有用CLI替代核心录制/提取实现。

| 场景 | 实际结果 |
|---|---|
| MP4并行分段 | 4段，合计80视频帧；与直播80包逐包SHA-256一致 |
| MKV并行分段 | 同样4段、80帧及包哈希一致 |
| 分段独立解码 | 所有段均H.264/AAC、首视频关键帧DTS=0、逐流DTS严格递增、完整解码通过 |
| 节目索引 | 每段写尾后登记，起点0/1/2/3秒，末段0.2秒也正确写尾 |
| 关键帧列表 | 单文件读取到0/1/2/3秒4个关键帧 |
| 文件内片段提取 | 请求1.4秒+0.7秒，实际起点1.0秒，终点2.1秒，28帧，可完整解码 |
| 索引定位 | 请求节目1.4秒定位第2段，0.4秒时长按局部关键帧展开为20帧 |
| 实时回放 | 1秒片段按DTS节奏发送，三种配置耗时见表 |
| 录制目录不存在 | 录制ERROR，直播继续完整80帧 |
| 录制容量1024字节 | 包超过可用容量，录制停止，直播仍完整80帧 |
| 已有索引 | 拒绝覆盖，录制失败隔离 |
| 第二段路径冲突 | 旧文件内容保持；第一段可解码；直播完整80帧 |
| SIGINT | 主程序退出130，录制线程排空写尾，部分MP4可解码 |
| 错误输入 | 越界时间、损坏媒体/索引、非法参数、跨段提取被拒绝 |
| 输出保护 | 提取到已有文件失败，原文件SHA-256不变 |

初次异常隔离测试失败于对直播FLV首视频零点的错误假定。
检查AAC预卷和avoid_negative_ts后，明确直播FLV首视频约21ms而分段首视频为0。
分别校验这两种时间语义后，完整11项在三种配置通过；未改变输出掩盖问题。

手工保留的演示产物（build不入Git）：

- build/samples/phase6-live.flv：3秒、75视频帧、142AAC包，直播包零丢失。
- build/samples/phase6-record-000000.mp4至000002.mp4：三个1秒录制段。
- build/samples/phase6-record.index.tsv：节目区间[0,1)、[1,2)、[2,3)秒与绝对文件路径。
- build/samples/phase6-highlight.mkv：请求节目[1.2,1.7)秒，从1秒IDR开始，18视频帧、33AAC帧。
- build/samples/phase6-replayed.flv：上述片段按DTS实时回放，完整解码退出0。

手工演示平均25.0857fps、编码数据141.267kbps，直播队列峰值1包/886字节，
排队/写入峰值211/86us。纯色低复杂度输入，不作为赛事画面的性能结论。
编译日志在build/phase6-build.log、phase6-release.log、phase6-asan.log；
Debug全测记录在build/phase6-debug-test.log，详细CTest输出在各构建目录Testing/Temporary/LastTest.log。
复现命令见README Phase 6，VSCode增加录制与提取配置，JSON语法已校验，界面F5仍未实测。

未验收：真实RTMP同时推流录制/回放接收播放、人工音画检查、真实磁盘写满、
持续慢磁盘、断电恢复、24小时稳定性及CPU/RSS长期趋势。
当前实现明确不提供跨段自动拼接、精确帧级裁剪或AAC采样级无缝分段。
未部署服务端、未提交Git，Phase 7未开始。


## Phase 7 验收记录（2026-10-29）

环境仍为Ubuntu24.04/WSL2、i7-14650HX（24逻辑CPU）、约15GiB内存、
GCC13.3、CMake3.28.3、FFmpeg6.1.1。perf版本6.18.54.1，perf_event_paranoid=2。
没有安装新依赖、改动系统性能权限或部署服务端。

### 构建、回归和资源检查

| 配置 | 最终全套CTest | 耗时 |
|---|---|---|
| Debug | 13/13通过 | 29.62秒 |
| Release | 13/13通过 | 29.34秒 |
| ASan+UBSan | 13/13通过 | 33.24秒 |

均编译成功，无编译警告；保持-std=c++11。ASan开启detect_leaks=1:halt_on_error=1，
UBSan开启halt_on_error=1，覆盖路径未报告错误。系统FFmpeg库未重新插桩。
新增metrics_integration验证四路真实媒体计数、CPU/RSS、计时、队列和文件保护，
video_reference验证源帧释放后的有效性及写时复制不会污染其他引用。
原有输出恢复、录制失败隔离、SIGINT、编解码和内容同步用例全部保留。

### 四路1080p/30fps软件采样及优化对照

输入为四个独立会话读取同一动态testsrc2+440Hz样片，输出1920×1080/30fps四宫格，
H.264 veryfast/zerolatency、目标1.2Mbps、GOP1秒、VBV500ms，AAC48kHz双声道，
同步写出FLV并录制MP4。输入视频使用ultrafast/CRF28生成，不代表真实赛事码流复杂度。
每次实际运行10秒，素材生成和末端解码检查不计入时间；测试间没有并行运行CTest。

| 指标 | 优化前 | 引用计数直通后 |
|---|---:|---:|
| 进程墙钟秒 | 10.157 | 10.156 |
| 输出视频帧 | 300 | 300 |
| 稳态窗口CPU均值（单核100%） | 76.982% | 63.519% |
| 采样当前RSS最大值KiB | 288340 | 243224 |
| RSS前后窗口中位数差KiB | 6624 | 424 |
| 采样线程数最大值 | 28 | 28 |
| 编码API累计微秒 | 1505932 | 1502446 |
| 解码API累计微秒 | 105894 | 126896 |
| 合成/混音累计微秒 | 1006395 | 863920 |
| CSV采样点调度落后最大微秒 | 13899 | 11344 |

两次均无输出丢包/调度跳过，实际FLV及录制解码通过。
CPU均值排除首个启动和最后收尾窗口，不是整段包含启动的平均CPU；RSS只是采样峰值。
只做一次前后配对，不宣称统计显著或固定百分比收益。API累计耗时不包括内部工作线程的全部CPU。

perf使用cpu-clock软件事件、99Hz；优化前712样本，优化后580样本，丢失均0。
优化前动态库占比：libavcodec38.34%、libx26429.92%、libswscale17.42%、libc12.22%；
优化后分别38.97%、36.90%、18.28%、3.79%。样本总数改变，占比不能直接等同绝对耗时变化。
优化依据是采样中的缩放/内存开销与代码中同尺寸YUV420P仍逐帧复制的事实；
改为av_frame_ref共享只读图像，非匹配尺寸/格式仍走swscale。

perf探测cycles/instructions返回not supported；本perf缺少libunwind/libdw支持，
系统库有缺失符号。记录到的软件地址/动态库样本可用，但不能声称完整调用链或具体libc函数定位。
没有为消除这些限制擅自安装系统依赖。

原始证据：build/phase7-1080-profile与phase7-1080-optimized下的command.json、
summary.json、metrics.csv、sync.csv、perf.data、perf-report.txt、perf-dso.txt、录制及日志。
最终回归日志为build/phase7-{debug,release,asan}-final.log。


### 四路1080p持续120秒实测

使用优化后的Release，无perf附加开销，四路1920×1080/30fps文件输入持续有效，
输出同尺寸四宫格、H.264/AAC，同时每10秒录制一个MP4。素材时长121秒，
每路最终解码3605视频帧和5642音频块，超过目标节目3600帧；末尾预读受有界队列限制。
同一个样片被4个独立会话读取，不能替代4路独立设备采集验收。

| 指标 | 实测 |
|---|---:|
| 墙钟运行秒 | 120.041 |
| 输出视频帧 | 3600 |
| 输出音频包 | 5626 |
| 平均输出fps（程序日志） | 30.0033 |
| 实际编码数据kbps（不含封装/网络开销） | 1259.112 |
| 稳态窗口CPU均值 / 最大值 | 64.266% / 69.955% |
| 采样RSS最大值 | 246304KiB（约240.5MiB） |
| RSS首尾窗口中位数差 | +2276KiB |
| 线程数采样最大值 | 28 |
| 输出队列实际峰值（日志） | 2包 / 34571字节 |
| 输出队列CSV采样最大值 | 1包 |
| 视频/音频输入队列采样上限 | 16帧 / 64块（各4路合计） |
| 逐包统计排队 / 写入峰值 | 8340us / 25887us |
| CSV采样点墙钟调度落后峰值 | 11094us |
| 输出丢包 / 调度跳过 | 均0 |
| 成功录制段数 | 12 |

FLV完整解码、全部12段MP4完整解码均退出0，程序正常结束，最终输出队列排空。
CSV只按秒采样，所以1包不等于整个运行中真实峰值；逐次更新的日志峰值为2包。
120秒中RSS窗口差约2.2MiB，不能据此判断是分配器预热、缓存还是长期趋势，
没有将该结果宣称为“无持续性内存增长”。线程数在输入join后下降，但编码器等
仍在返回前保持正常上下文；最终进程退出与ASan覆盖路径检查均通过。

结果目录build/phase7-1080-soak约201MiB，包含原始样片、输出、分段、CSV和summary.json。
复现：README Phase 7的120秒命令，必须使用新的结果目录。

**仍未验收：** 四路1080p连续24小时、真实RTMP成功会话及实际中途断网恢复、
播放器端到端延迟、人工视听、真实设备长期时钟漂移、磁盘写满/断电和长期RSS趋势。
既有故障注入恢复测试仍通过，但不等价于上述实际环境验收。
本轮没有提交Git，没有部署服务端；Phase 7实现与短时验证完成，长期目标保持未完成。

## Phase 7 退出保护补充（2026-10-30）

本轮复查压力脚本，发现perf包装器超时只终止直接子进程的风险，以及音频持续输入
只判断曾有帧的弱校验。新增scripts/process_runner.py与process_runner CTest，
为外部命令使用独立进程组，温和SIGINT后有界等待，再按需SIGKILL并检查后代状态。
ffprobe、perf报告也加入超时；48kHz/AAC测试素材按目标时长检查每路音频块数。
未修改C++媒体链路、未安装依赖、未部署服务端。

| 检查 | 实际结果 |
|---|---|
| Debug完整CTest | 14/14通过，34.01秒 |
| 最终增加真实媒体超时后Debug相关复测 | 2/2通过，7.45秒 |
| Release配置/构建及相关复测 | 成功，最终2/2通过，7.50秒 |
| ASan+UBSan配置/构建及相关复测 | 成功，最终2/2通过，7.56秒 |
| 包装器先退出，后代忽略SIGINT | 超时仍报失败，后代被强制停止执行 |
| 子进程可温和停止 | SIGINT回调完成收尾标记，保留原超时异常 |
| 对工具发送Ctrl+C | 包装器与后代停止执行，工具非零退出 |
| 正常退出/非零退出 | 正常0返回，错误码7被保留 |
| 真实四路媒体1.2秒超时 | FLV和部分MP4写尾，均完整解码成功 |
| 持续音频计数与文件保护 | 收紧门槛后仍通过，已有指标文件不覆盖 |

初次进程组测试失败于信号投递和后代实际退出之间的竞态。并非跳过该测试，
而是增加有界/proc进程状态确认后复测通过。Z/X表示不再执行，系统父进程可能
稍后才回收孤儿僵尸；不能将发送SIGKILL本身当作全部后代已经停止。

ASan使用detect_leaks=1:halt_on_error=1，UBSan使用halt_on_error=1，覆盖媒体路径未报告错误。
Python生命周期代码不由ASan检查，使用进程状态、退出码和媒体解码结果验证。
本次最终只增加测试工具及其用例，Release/ASan复测相关两项；没有把2/2写成完整14/14。
日志位于build/hardening-20261030-full.log、targeted-final.log、release-final.log和asan-final.log
（后三个文件同样带hardening-20261030-前缀）。

本轮没有重跑120秒或24小时负载；既有120秒结果保留为历史记录。
真实RTMP成功会话、24小时稳定性、真实磁盘写满和断电恢复仍未完成验收。
README、开发计划、中文学习笔记、架构说明均已更新；没有Git提交。

## Phase 7 真实文件写失败补充（2026-10-30）

继续补录制开始写入后的错误路径。新增recording_write_failure_fixture（C++11）
和recording_write_failure（Python/CTest），真实编码100帧动态YUV视频与AAC音频，
一支路由Recorder写MP4，另一支路由FFmpeg C API写FLV到stdout管道。
独立子进程使用RLIMIT_FSIZE=4096，内核真实返回EFBIG；没有填满实际磁盘。
父进程接收FLV字节，再检查包哈希和完整解码，测试没有替换成功的FFmpeg调用。

实测结果：

- 不限制文件大小的对照组：MP4写尾、登记1段索引、完整解码通过。
- 限制组：MP4不能超过4096字节，日志包含File too large，失败段未登记，没有“录制完成”伪报。
- 两组主FLV均有100视频帧，与对照组逐包SHA-256一致，音视频完整解码通过。
- 实际esports_live额外测试初次退出−25，定位为SIGXFSZ直接终止。
- 修复esports_live信号策略后，主输出文件上限被明确报告为错误，退出1，资源正常释放。

| 配置 | 最终完整CTest | 耗时 |
|---|---|---|
| Debug | 15/15通过 | 34.34秒 |
| Release | 15/15通过 | 34.26秒 |
| ASan+UBSan | 15/15通过 | 38.67秒 |

三种配置构建成功。ASan开启detect_leaks=1:halt_on_error=1，UBSan开启halt_on_error=1，
覆盖路径未报告错误。完整回归包含先前新增的进程组退出、真实媒体超时、原有网络失败恢复等测试。
日志：build/record-write-20261030-{debug,release,asan}-final.log。

边界：EFBIG来自进程文件大小限制，不是磁盘耗尽ENOSPC；本地FLV管道成功不等于RTMP成功会话。
真实磁盘写满、断电、24小时长跑和真实网络验收继续保留为未完成。
没有部署服务端，没有Git提交。

## JSON启动配置补齐（2026-10-30）

实现--config、--check-config、--dump-config，以及--realtime/--unmute反向覆盖。
新增esports_config目标，私有使用随源码提供的PicoJSON v1.3.0单头文件及许可证；
构建无网络下载、无新增系统安装步骤，Debug/Release明确包含-std=c++11。
config/server.json不再是预留对象，仓库示例已实际运行。

| 配置 | 最终完整CTest | 总耗时 |
|---|---|---|
| Debug | 16/16通过 | 36.40秒 |
| Release | 16/16通过 | 36.26秒 |
| ASan+UBSan | 16/16通过 | 42.06秒 |

三个配置构建成功，未出现编译警告。ASan使用detect_leaks=1:halt_on_error=1，
UBSan使用halt_on_error=1，覆盖路径未报告错误。
Release对修正后的recording_write_failure另连续重复5次，全部通过，总计2.69秒。

新增config_integration验证：

- 默认值→JSON→CLI优先级，--config参数位置无关，覆盖后进行最终数值校验。
- CLI输入列表整体替换，逐索引增益/静音/偏移覆盖，realtime/unmute反向覆盖。
- dump导出后重新加载，得到同样有效配置；check/dump不创建输出。
- 未知字段、重复键、类型/范围错误、字段关系错误拒绝。
- 畸形JSON/数字、超长数字、尾随内容、无效UTF-8、NUL、超1MiB文件、FIFO拒绝。
- JSON实际驱动合成/音频/录制/CSV，CLI覆盖fps得到25帧，中文路径和已有输出保护通过。

手工运行仓库server.json：输出640×360/H.264视频75帧、AAC142帧，3秒真实节奏，
音视频零输出丢包，录制三个MP4分段，索引区间[0,1)、[1,2)、[2,3)秒。
产物为build/samples/config-live.flv、config-record-000000.mp4至000002.mp4，
config-sync.csv与config-metrics.csv；主FLV完整解码成功。分段解码由集成测试覆盖。
日志为build/config-example.log，复现命令与字段表见docs/configuration.md。
示例已经运行产生文件，复跑须换用未使用的输出、录制及CSV路径。

本轮首次Release全测在旧recording_write_failure用例出现跨运行包哈希不一致。
32次并发且均无故障的对照运行也出现多个不同哈希，证明不能要求独立编码运行字节确定。
用例改为保存每次运行分流前的编码包摘要，与该次FLV包逐一比较；比较前将Annex B
起始码规范成FLV使用的AVCC长度表示，否则合法封装转换本身也会造成哈希不同。
没有跳过完整性检查，仍要求100帧、全部包对应、完整解码、真实EFBIG和失败索引不登记。
故障调查文件保存在build/config-write-parallel-92ycvq4j；最终结果为上述16/16。

最终日志：build/config-{debug,release,asan}-accepted.log。
README、configuration.md、开发计划、学习笔记、架构及VSCode配置均已更新，JSON格式检查通过。
配置仅启动时读取，不支持热更新；check/dump不验收输入可读性或网络连接。
真实RTMP、24小时与其他既有未验收项保持未完成；没有Git提交。

## 2026-10-31：中文源码注释维护回归

此次修改覆盖70个自有代码/构建文件，新增306行注释；src/include所有文件均补充模块职责，
核心交接点注明线程、资源所有权、时间基、背压和异常恢复。测试与脚本增加验证目的说明。
新增docs/code-reading-guide.md，并同步README、学习笔记和开发计划；未修改第三方源码。

变更前后逐行比较确认代码/构建文件仅新增注释、没有删除或改写原有语句；
Python AST比较一致，Shell语法检查、阅读指南链接检查及git diff --check通过。

| 验证 | 真实结果 | 日志 |
|---|---|---|
| Debug构建 | 成功，退出0，无warning/error诊断 | build/comment-audit/debug-build.log |
| Release构建 | 成功，退出0，无warning/error诊断 | build/comment-audit/release-build.log |
| Debug CTest | 16/16通过，36.57秒 | build/comment-audit/debug-ctest.log |
| Release CTest | 16/16通过，34.59秒 | build/comment-audit/release-ctest.log |
| 仅注释变更检查 | 70文件、306行；原语句保留 | build/comment-audit/change-check.txt |

本轮未重复ASan/UBSan或压力测试；未运行真实网络验收，也不把文档维护视为新增媒体能力。
此前长期稳定性、真实设备/网络等未验收项仍然保留。未进行Git提交。

## 2026-11-09：交付代码收敛验证

删除旧阶段独立程序及专用管理器，LiveOptions解除阶段类型继承，视频输入仅保留完整节目共同原点模式，
移除重复音频转换器及未使用的open_encoded，视频转换文件改名为video_converter，环境入口改为env_check_main。

测试迁移说明：旧Remux产品专属包透传断言不再适用；其输入拒绝、握手超时和停止能力迁到
input_integration。旧转码/合成测试迁成codec_integration与video_layout_integration，直接调用esports_live。
编解码测试除检查输出帧数，还核对每路解码计数，防止节目重复帧掩盖B帧尾部未排空。
布局测试明确2秒节目长度，继续严格检查40帧及四色像素，原因见学习笔记。

| 验证 | 真实结果 | 日志 |
|---|---|---|
| Debug配置/构建 | 成功，无编译警告 | build/consolidation-check/debug-config.log、debug-build.log |
| Release配置/构建 | 成功，无编译警告 | build/consolidation-check/release-config.log、release-build.log |
| Debug CTest | 16/16通过，28.16秒 | build/consolidation-check/debug-tests.log |
| Release CTest | 16/16通过，28.08秒 | build/consolidation-check/release-tests.log |
| ASan/UBSan配置/构建及CTest | 构建成功，16/16通过，32.33秒，覆盖路径未报告内存/未定义行为错误 | asan-config.log、asan-build.log、asan-tests.log |
| 编解码测试最终修订 | Debug/Release各1/1通过；输出保护场景关闭CSV，确保验证媒体路径冲突 | debug-codec-final.log、release-codec-final.log |
| 无测试依赖的全新Release构建与安装 | BUILD_TESTING=OFF成功，bin恰含live/replay/env_check三个工具 | production-build.log、install.log |
| 安装后实际运行 | 三份配置校验成功；3秒节目75帧H.264/142帧AAC，录制后提取MKV；FLV/MKV完整解码成功 | delivery-check.log及installed-run-bsljzhzk目录 |

上述相对日志名均位于build/consolidation-check。本轮未做真实网络成功会话和长期压力验收。
已删除build/debug、release、asan根目录中旧三程序的ELF产物，未清除历史素材和测试报告。
历史报告中的阶段程序/路径属于当时记录，不再作为当前运行指引。
