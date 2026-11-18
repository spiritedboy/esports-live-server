# Esports Live Server 完整部署指南

日期：2026-11-18。适用于当前完整直播、录像回放和环境检测三个程序。

建议依次完成：安装依赖 → 编译测试 → 安装到独立目录 → 本地验收 → 配置真实输入输出 → 前台试运行 → 可选systemd托管。
本文中的系统安装命令供部署时执行，本轮文档编写不会替你创建系统账户、修改服务或部署RTMP服务端。

## 1. 部署的是什么

```text
上游录屏/摄像头/麦克风采集程序或本地文件
              ↓ 文件、RTMP拉流、RTSP TCP拉流
       esports_live：解码、同步、合成、混音、编码
              ├─ 本地FLV
              ├─ RTMP推送 → 已有接收服务 → 播放器
              └─ MP4/MKV分段录像与CSV报告
```

本项目不直接采集桌面、USB摄像头或麦克风设备，也不是监听1935端口的RTMP服务器。OBS需要推给已有媒体服务，再由本程序拉取。
本文不安装SRS，不实现WebRTC，也不提供网页控制台或HTTP健康检查接口。

| 安装程序 | 用途 |
|---|---|
| esports_live | 制作完整节目并输出H.264+AAC |
| esports_replay | 本地录像关键帧查询、提取、实时回放 |
| ffmpeg_env_check | 检查FFmpeg库版本和注册能力 |

输入视频支持H.264/H.265，音频支持AAC/Opus；直播要求2或4路视频。默认软件编解码，不依赖GPU。

## 2. 环境准备

### 2.1 Ubuntu与WSL2

优先使用Ubuntu 24.04系列及FFmpeg 6.1.x。Ubuntu Noble官方包目录提供6.1.1系列开发库，实际安全更新版本以目标机apt查询为准。
不要混用来自不同安装前缀的头文件和动态库。[Ubuntu包目录](https://packages.ubuntu.com/noble/libdevel/)

在Ubuntu终端执行：

```bash
cat /etc/os-release
uname -m
getconf _NPROCESSORS_ONLN
free -h
df -h .
gcc --version
g++ --version
cmake --version
```

尚未安装的命令报错时，继续下一节安装依赖即可。
没有经基准测试确认的统一最低硬件规格：源数、分辨率、帧率和输入编码都会影响负载。先用640×360、25fps完成验收，再逐步提高到目标规格。

WSL2建议把源码、构建和录像放在Linux文件系统内，例如`~/codex/esports-live-server`，避免把大量媒体读写放在`/mnt/c`共享目录。
电脑休眠、Windows重启或WSL关闭会中断节目。长期值守应使用持续在线的Linux环境，并单独验收稳定性。

### 2.2 安装依赖

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config gdb git python3 ffmpeg \
  libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev \
  libswresample-dev libswscale-dev
```

用途：GCC/G++编译C++11，CMake生成构建系统，pkg-config发现FFmpeg六个库，Python/ffmpeg/ffprobe运行自动化测试。
CMake生成的Makefile是正常构建产物，不需要手写Makefile。

```bash
pkg-config --modversion libavformat libavcodec libavutil libavfilter libswresample libswscale
ffmpeg -version
ffmpeg -hide_banner -encoders
apt-cache policy ffmpeg libavcodec-dev
```

编码列表中应有libx264和aac；完整测试素材还用到libx265和libopus。
若apt找不到包，先检查目标Ubuntu发行版与软件源，部分包位于universe；不要以更换整套媒体框架解决依赖问题。
若目标系统的FFmpeg不是6.1.x，先单独评估兼容性；当前CMake只负责发现库，并不强制锁定FFmpeg版本。

## 3. 获取源码与记录版本

已有工作目录就直接进入，不要重新clone覆盖或丢弃本地修改：

```bash
cd ~/codex/esports-live-server
git status --short
git log -1 --oneline
```

仅当目标机器没有仓库时，才在所选父目录执行：

```bash
git clone https://github.com/spiritedboy/esports-live-server.git
cd esports-live-server
```

部署前记录commit及工作区是否干净。不要将带未记录本地修改的程序标记为某个纯commit版本。
后续命令除特别注明外，均在仓库根目录运行。

## 4. 编译与自动化测试

### 4.1 Debug：用于开发排查

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel 4
./build/debug/ffmpeg_env_check
ctest --test-dir build/debug --output-on-failure
```

### 4.2 Release：用于部署

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release --parallel 4
./build/release/ffmpeg_env_check
ctest --test-dir build/release --output-on-failure
```

内存不足时将并行度4降为2或1。CMake强制C++11并禁用语言扩展。
当前共有16项CTest。失败时先看`ctest --output-on-failure`和`build/release/Testing/Temporary/LastTest.log`，修复后再安装。

环境检测退出0只表示其版本兼容性检查通过；仍须看libx264/AAC/FLV/RTMP能力行。缺能力时程序可能只警告，不能仅凭退出码宣布可推流。
自动化恢复测试使用故障注入，不能代替真实RTMP接收播放验收。

仅构建运行工具时可使用下面的独立目录，但这不会执行任何测试：

```bash
cmake -S . -B build/runtime -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/runtime --parallel 4
```

## 5. 安装到独立目录

先做普通用户可写的安装验收，无需sudo。下面变量需在同一个终端保留：

```bash
mkdir -p "$PWD/build/deploy-check"
deploy_root=$(mktemp -d "$PWD/build/deploy-check/release.XXXXXX")
cmake --install build/release --prefix "$deploy_root"
"$deploy_root/bin/ffmpeg_env_check"
ldd "$deploy_root/bin/esports_live"
```

检查ldd中没有`not found`。安装内容为：

```text
安装目录/
├── bin/
│   ├── esports_live
│   ├── esports_replay
│   └── ffmpeg_env_check
└── share/esports-live-server/config/
    ├── server.json
    ├── server.full-example.json
    └── server.network-example.json
```

CMake不安装测试夹具、文档或scripts目录；它们留在源码仓库。
FFmpeg动态库也不会打包进安装目录。把二进制复制到另一台机器前，确认CPU架构、系统运行库与FFmpeg ABI匹配；优先在目标环境或兼容构建环境生成程序。

## 6. 必须先完成一次本地验收

继续使用上一节的deploy_root，在同一终端、仓库根目录执行。
每次用mktemp建新目录，避免覆盖录制、CSV和输出文件：

```bash
run_dir=$(mktemp -d "$PWD/build/deploy-check/local.XXXXXX")
bash scripts/generate-mix-samples.sh "$run_dir/mix"
bash scripts/generate-audio-samples.sh "$run_dir/audio"

python3 - "$run_dir" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
config = {
    "inputs": [str(p / "mix/red.mp4"), str(p / "mix/lime.mp4")],
    "audio_inputs": [str(p / "audio/game.m4a"), str(p / "audio/voice.opus")],
    "output": str(p / "program.flv"),
    "layout": "pip", "width": 640, "height": 360, "fps": 25,
    "duration_ms": 3000, "realtime": True,
    "record_prefix": str(p / "record"), "record_format": "mp4", "segment_ms": 1000,
    "stats_file": str(p / "sync.csv"), "metrics_file": str(p / "metrics.csv")
}
(p / "server.json").write_text(json.dumps(config, indent=2), encoding="utf-8")
PY

"$deploy_root/bin/esports_live" --config "$run_dir/server.json" --check-config
"$deploy_root/bin/esports_live" --config "$run_dir/server.json" \
  > "$run_dir/live.log" 2>&1
ffprobe -v error -show_entries stream=codec_name,width,height,sample_rate,channels \
  -of json "$run_dir/program.flv"
ffmpeg -v error -xerror -i "$run_dir/program.flv" -f null -
cat "$run_dir/record.index.tsv"
"$deploy_root/bin/esports_replay" --input "$run_dir/record-000000.mp4" \
  --start-ms 0 --duration-ms 500 --output "$run_dir/clip.mkv"
ffmpeg -v error -xerror -i "$run_dir/clip.mkv" -f null -
printf '本轮输出目录：%s\n' "$run_dir"
```

期望：程序正常退出，FLV含H.264/AAC，ffmpeg完整解码无错误，录像索引含已完成片段，提取的MKV可以解码。
再实际打开FLV观看画中画、听两路测试音；命令行解码通过不能代替人工视听。

--check-config只验证字段、范围及组合，不检查文件存在、端点可达或设备是否在线。
不要直接重复运行同一个server.json：output、record_prefix、stats_file、metrics_file都需要新的路径。上面从run_dir开始重新执行即可生成全新一轮。

## 7. 换成真实输入和RTMP输出

先准备以下地址：电脑画面拉流地址、摄像头RTSP地址、主持人音频拉流地址，以及已有RTMP服务的发布地址。
本进程主动连接这些端点，通常无需为它开放入站监听端口；网络策略须允许访问地址中实际配置的端口。它不会替你建立RTMP接收服务。

下面是教学配置，192.0.2.x是占位地址，必须全部替换。可保存为新文件`build/network-deploy.json`，不要直接覆盖现有配置。

```json
{
  "inputs": [
    "rtmp://192.0.2.10/live/screen",
    "rtsp://192.0.2.20:554/stream1"
  ],
  "audio_inputs": [
    "rtmp://192.0.2.10/live/screen",
    "rtmp://192.0.2.10/live/host"
  ],
  "output": "rtmp://192.0.2.30/live/program",
  "layout": "pip",
  "width": 1280,
  "height": 720,
  "fps": 25,
  "duration_ms": 0,
  "realtime": true,
  "gains": [0.3, 1.0],
  "muted": [false, false],
  "video_offsets_ms": [0, 0],
  "audio_offsets_ms": [0, 0],
  "stale_ms": 500,
  "timeout_ms": 5000,
  "video_queue_frames": 4,
  "audio_queue_blocks": 16,
  "video_bitrate": 1200000,
  "gop_ms": 1000,
  "vbv_ms": 500,
  "output_timeout_ms": 3000,
  "reconnect_attempts": 5,
  "retry_backoff_ms": 250,
  "output_latency_ms": 500,
  "packet_queue_count": 256,
  "packet_queue_bytes": 8388608,
  "record_prefix": "",
  "record_format": "mp4",
  "segment_ms": 60000,
  "record_queue_bytes": 16777216,
  "stats_file": "",
  "metrics_file": ""
}
```

关键设置：

- duration_ms=0表示无指定节目时长；有限文件读完仍会结束，不会自动循环。
- RTMP必须realtime=true。当前只支持明文rtmp://，不要自行改成未支持的rtmps://或其他协议。
- audio_inputs显式指定电脑声和主持人声；不要重复混入摄像头麦克风。空数组表示继承视频URL，不是禁音。
- 偏移0只是起点，现场同步按[三来源音画同步方案](screen-camera-microphone-sync.md)测量后调整。
- JSON不展开环境变量、`~`或`${...}`，也不支持注释或热更新。相对媒体路径相对进程工作目录，不是配置文件目录。
- 配置可能包含凭据，限制读取权限。--dump-config和底层媒体错误日志可能显示URL，不要直接公开原始输出。

前台验证：

```bash
chmod 600 build/network-deploy.json
./build/release/esports_live --config build/network-deploy.json --check-config
./build/release/esports_live --config build/network-deploy.json
```

日志显示输出会话建立之后，还要用接收端和播放器确认画面/声音、时间戳与实际延迟。发布地址和播放地址可能不同，以现有服务为准。
先短时正常运行，再检查输入断开、输出短时中断、恢复预算耗尽及停止行为。不要把看到一个“连接成功”日志当成全链路验收。

当前音视频分别打开同一URL，设备可能承受两次拉流连接。输入断流只降级黑帧/静音，没有自动输入重连；输出重连次数是整个输出生命期累计预算，成功连接不会清零。

## 8. 正常启动、停止和退出码

前台运行按Ctrl+C。代码尝试停止输入、回收线程和完成写尾；不要用kill -9作为常规停止方法。

| 情况 | 处理方式 |
|---|---|
| 正常媒体结束 | 通常退出0 |
| 用户停止 | 媒体主流程通常返回130；部分启动/异常路径可能返回1，应结合日志判断 |
| 输入或输出等不可继续错误 | 非零退出，先看错误上下文 |
| 录制支路单独失败 | 主直播可能继续并最终退出0，必须另查录制日志和索引 |

程序没有PID文件、后台守护选项或API控制接口。前台阶段不要关闭终端；长期运行使用下一节托管。
停止期间仍可能等待录制线程排空。任意内核磁盘阻塞不保证固定时间内退出，强制终止可能损坏正在写入的片段。

## 9. 可选：安装成Linux系统服务

只有本地验收与真实端点前台验收通过后再做。以下是单实例部署模板，需要sudo；不要在仍有旧节目运行时直接切版本。

### 9.1 目录与账户

```text
/opt/esports-live-server/releases/<版本>/   只读程序与示例配置
/opt/esports-live-server/current           指向当前版本的符号链接
/etc/esports-live-server/server.json       真实配置
/usr/local/libexec/esports-live-start      每次启动分配输出目录
/var/lib/esports-live-server/runs/          录像、索引和CSV
```

在仓库根目录、干净工作树下选唯一版本名，例如commit加部署批次；若有本地修改，先记录来源而非冒充纯commit：

```bash
deploy_version="$(git rev-parse --short=12 HEAD)-$(date -u +%Y%m%dT%H%M%SZ)"
getent passwd esports-live >/dev/null || \
  sudo useradd --system --user-group --home-dir /var/lib/esports-live-server \
    --shell /usr/sbin/nologin esports-live
sudo install -d -m 0755 /opt/esports-live-server/releases /usr/local/libexec
sudo install -d -m 0750 -o root -g esports-live /etc/esports-live-server
sudo install -d -m 0750 -o esports-live -g esports-live \
  /var/lib/esports-live-server /var/lib/esports-live-server/runs
sudo test ! -e "/opt/esports-live-server/releases/$deploy_version"
sudo cmake --install build/release --prefix "/opt/esports-live-server/releases/$deploy_version"
sudo ln -sfnT "/opt/esports-live-server/releases/$deploy_version" /opt/esports-live-server/current
```

如果账户已存在，先确认它确实是本项目专用账户、同名组存在，不能直接复用其他业务账户。
上面的test若失败应停止，换一个新版本目录，不继续覆盖安装。

首次安装真实配置：

```bash
sudo test ! -e /etc/esports-live-server/server.json
sudo install -m 0640 -o root -g esports-live build/network-deploy.json \
  /etc/esports-live-server/server.json
sudo -u esports-live /opt/esports-live-server/current/bin/ffmpeg_env_check
sudo -u esports-live /opt/esports-live-server/current/bin/esports_live \
  --config /etc/esports-live-server/server.json --check-config
```

若已有配置，先备份并人工合并，不能继续执行覆盖命令。真实配置使用绝对本地路径；相关输入的文件与父目录须允许esports-live读取/穿越。

### 9.2 为每次启动创建独立录像和报告目录

本启动脚本要求配置中的output为RTMP地址，且真实端点已通过前台验收。脚本启用录制和CSV，用CLI覆盖配置的对应路径。
这解决固定文件名导致第二次启动失败的问题。若不录制，把`--record-prefix "$run_path/record"`改为`--record-prefix ""`。

将下列内容保存为`/usr/local/libexec/esports-live-start`（例如用`sudoedit`新建，已有脚本应先备份）：

```bash
#!/usr/bin/env bash
set -euo pipefail
umask 027
run_path=$(mktemp -d /var/lib/esports-live-server/runs/session.XXXXXXXX)
printf '本次节目目录：%s\n' "$run_path" >&2
exec /opt/esports-live-server/current/bin/esports_live \
  --config /etc/esports-live-server/server.json \
  --record-prefix "$run_path/record" \
  --stats-file "$run_path/sync.csv" \
  --metrics-file "$run_path/metrics.csv"
```

```bash
sudo chown root:root /usr/local/libexec/esports-live-start
sudo chmod 0755 /usr/local/libexec/esports-live-start
sudo bash -n /usr/local/libexec/esports-live-start
```

若改为本地FLV输出，必须在exec参数中另外加`--output "$run_path/program.flv"`，不能使用配置中固定的已有输出文件。
脚本用exec替换自身，服务管理器直接跟踪媒体进程，并将停止信号送给它。

### 9.3 systemd单元

将以下内容保存为`/etc/systemd/system/esports-live.service`：

```ini
[Unit]
Description=Esports Live Server
Wants=network-online.target
After=network-online.target
StartLimitIntervalSec=300
StartLimitBurst=3

[Service]
Type=simple
User=esports-live
Group=esports-live
WorkingDirectory=/var/lib/esports-live-server
UMask=0027
ExecStartPre=/opt/esports-live-server/current/bin/esports_live --config /etc/esports-live-server/server.json --check-config
ExecStart=/usr/local/libexec/esports-live-start
KillSignal=SIGTERM
TimeoutStopSec=120
SuccessExitStatus=130
Restart=no
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

默认不自动重启，先观察故障原因。确认配置、磁盘容量与业务恢复策略后，可以将Restart改为on-failure并增加RestartSec=5；示例Unit已设置启动频率限制。
退出0或130不会因此自动重启。进程活着但输入已降级也不会触发重启，因此systemd不能替代媒体健康监控。

TimeoutStopSec是停止等待上限，超过后服务管理器可能强制终止进程；这里的120秒是部署示例，不是代码保证。
Type=simple的active只表示进程启动，不表示已经推流成功。相关指令语义见[systemd官方手册](https://www.freedesktop.org/software/systemd/man/255/systemd.service.html)。

```bash
sudo systemd-analyze verify /etc/systemd/system/esports-live.service
sudo systemctl daemon-reload
sudo systemctl start esports-live
sudo systemctl status esports-live --no-pager
sudo journalctl -u esports-live -n 100 --no-pager
sudo journalctl -u esports-live -f
```

跟随日志时Ctrl+C只退出journalctl，不停止服务。确认验收通过再启用开机启动：

```bash
sudo systemctl enable esports-live
sudo systemctl stop esports-live
sudo systemctl start esports-live
```

restart会中断当前节目并建立新会话，不是无缝切换。修改JSON需要重启进程；修改单元还要先daemon-reload。

### 9.4 WSL2的systemd检查

```bash
ps -p 1 -o comm=
```

若不是systemd，先按微软文档检查WSL版本与支持情况；需要启用时，保留现有`/etc/wsl.conf`设置，合并：

```ini
[boot]
systemd=true
```

然后在Windows PowerShell执行`wsl --shutdown`并重新进入发行版。该操作会中断正在运行的WSL任务，不能在节目运行中执行。
启用步骤见[Microsoft WSL systemd文档](https://learn.microsoft.com/en-us/windows/wsl/systemd)。不启用systemd也可完成前台运行章节，但不能直接照抄systemctl命令。

## 10. 日常监控、录像与容量

每天至少观察实际接收画面/声音、输入失败日志、输出重试/丢包、CPU/内存、磁盘剩余容量和录制完成索引。

```bash
sudo journalctl -u esports-live --since '1 hour ago' --no-pager
df -h /var/lib/esports-live-server
du -sh /var/lib/esports-live-server/runs
systemctl show esports-live -p MainPID -p ActiveState -p SubState -p ExecMainStatus
```

每次启动路径在journal中打印。metrics.csv看处理耗时、队列和丢弃；sync.csv看节目进度与输出。但av_delta_us不是嘴型误差，必须结合实际内容检查。
当前无自动磁盘保留策略或报告轮转；不要让录像无限占满磁盘，也不要对正在写的CSV用普通日志轮转随意截断。

粗略估算：视频1.2Mbit/s加音频128kbit/s，编码数据约0.60GB/小时（十进制），另加容器、报告和码率波动余量。
本地FLV与录像同时保存会额外占用一份媒体空间，单独RTMP发送不会在本地保存该份网络输出。分辨率不直接决定容量，实际编码码率才决定主要增长速度。

索引只列出写尾成功的片段。录制失败不必然使主程序失败，需看日志是否出现“录制已停止”以及是否有完成记录。
备份录像时同时备份index.tsv；索引含绝对路径，迁移目录后需修正。当前没有断电恢复或自动修复未完成MP4的能力。

回放示例（把路径替换为实际已经完成的一段）：

```bash
/opt/esports-live-server/current/bin/esports_replay \
  --input /var/lib/esports-live-server/runs/session.EXAMPLE/record-000000.mp4 \
  --list-keyframes
```

提取和回放完整参数见[README录制与回放](../README.md#录制与回放)。回放输出也禁止覆盖；提取不跨分段自动拼接，不会插入正在直播的节目。

## 11. 更新与回滚

1. 保存当前版本链接目标、配置副本及测试记录：`readlink -f /opt/esports-live-server/current`。
2. 在独立源码/构建目录构建候选版本，跑测试并安装到新的releases目录，不覆盖正在运行版本。
3. 用候选程序检查配置，并完成新目录中的本地媒体验收。
4. 安排中断窗口，`sudo systemctl stop esports-live`，确认进程结束。
5. 将current链接切到候选版本；若配置需迁移，保留旧配置副本后更新。
6. 启动服务，检查新输出目录、RTMP接收和实际媒体。
7. 若失败，停止候选程序，恢复上一版本链接及配套配置，再启动。不要删除失败现场。

切换链接命令形式为`sudo ln -sfnT /opt/esports-live-server/releases/已验证版本 /opt/esports-live-server/current`，版本名需人工替换。
只切可执行文件不能回滚系统FFmpeg升级；如果变更了运行库，回滚计划还必须包含兼容运行环境。

## 12. 常见问题定位

| 现象 | 优先检查 |
|---|---|
| pkg-config找不到某库 | 对应-dev包、pkg-config搜索路径、是否混用了/usr与/usr/local |
| 编译提示FFmpeg API不存在 | 实际头文件版本是否为预期6.1.x；不要靠强制转型掩盖API差异 |
| 动态库not found | ldd、目标机依赖是否齐全、ABI是否匹配 |
| 缺少libx264 | 运行库的编码器注册表，不能只检查ffmpeg命令文件存在 |
| 配置通过但打开输入失败 | check-config不访问媒体；再查绝对路径、权限、URL、认证、网络 |
| 第二次启动提示文件已存在 | 四类输出路径是否复用；使用独立运行目录，不直接删原录像 |
| 所有源均失败/无媒体 | 源数、格式、端点就绪状态、超时；用日志定位具体一路 |
| 摄像头黑屏而节目仍运行 | 输入失败或画面过期；当前不自动重连输入，恢复源后需评估重启节目 |
| RTMP有限重试后退出 | 发布权限、地址、连接稳定性；重试预算累计且有上限 |
| CPU高、输出跳帧 | 先用低分辨率/帧率定位，再看metrics；盲目加大队列会增加延迟 |
| 嘴型错位 | 校准逐路偏移与采集时钟；不是调大stale_ms或修改gain |
| 录制没了但直播还在 | ERROR日志、录像队列、磁盘空间、目录权限和索引完成状态 |
| CSV写失败后节目退出 | 诊断文件写错误当前会终止主链路，检查容量与写权限 |
| systemctl显示active但无画面 | 只表示进程存活；检查日志、源是否降级及接收端 |
| 服务反复启动后被限制 | 先修错误，再用systemctl reset-failed esports-live清除限制后启动 |
| WSL中systemctl不可用 | PID1及WSL systemd设置；先使用前台方式 |

## 13. 验收清单与本轮验证记录

部署到目标机后逐项记录，不能用开发机结果替代：

- [ ] 构建版本、系统版本、FFmpeg版本及配置已记录。
- [ ] Release构建、环境能力和16项CTest通过。
- [ ] 安装目录中的程序能完成本地合成、混音、录制及提取解码。
- [ ] 真实摄像头/录屏/麦克风均可输入，实际内容已校准。
- [ ] RTMP接收端和播放器验证成功，不只检查推流进程日志。
- [ ] 输出中断恢复、输入失败降级、Ctrl+C/服务停止和重新启动已验证。
- [ ] CPU/内存/磁盘足以承受目标负载，录像保留与人工告警流程明确。
- [ ] 长时间运行和端到端延迟已在实际设备与网络条件下验收。

本轮实测结果见下方更新记录；未实际安装系统服务，不部署SRS或其他RTMP服务器。

2026-11-18在当前Ubuntu 24.04环境实测：

- Release重新配置与编译成功，16/16项CTest通过，耗时28.17秒。
- 直接提取本文第5、6节命令执行：新目录安装、安装后二进制环境检测、ldd检查和3秒本地节目通过。
- 输出FLV包含H.264/AAC，完整解码通过；生成三个已完成MP4段，逐段解码通过；MKV片段提取及解码通过。
- 全部bash片段通过bash -n语法检查；网络示例通过--check-config，未尝试连接占位地址。
- 原始构建/测试/本地验收日志及产物位置记录在build/deployment-guide/；样片与安装位于build/deploy-check/新建子目录。
- 本轮未重跑Debug/ASan、未进行人工视听、未创建系统账户或安装/启动systemd服务，未执行真实RTMP/RTSP和长期压力测试。

systemd章节为待目标机验证的部署模板；bash语法正确不代表系统权限、服务生命周期和真实媒体链路已验收。

