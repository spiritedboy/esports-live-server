#!/usr/bin/env python3
# 把可注入传输故障与真实客户端连接失败分开验证，无响应TCP端口不是媒体服务端。
"""完整程序：真实编码/封装故障注入，以及实际 RTMP 客户端失败路径；不部署 RTMP 服务端。"""
import argparse
import csv
import json
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time

p = argparse.ArgumentParser()
for name in ["program", "fixture", "ffmpeg", "ffprobe", "work"]:
    p.add_argument("--" + name, required=True)
a = p.parse_args()
Path(a.work).mkdir(parents=True, exist_ok=True)


def run(command, expected=0, timeout=20):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=timeout)
    if r.returncode != expected:
        raise RuntimeError(f"退出{r.returncode}，期望{expected}: {command}\n{r.stdout}\n{r.stderr}")
    return r


def require(value, message):
    if not value:
        raise RuntimeError(message)


with tempfile.TemporaryDirectory(dir=a.work, prefix="output-") as directory:
    root = Path(directory)
    sessions = root / "sessions"
    sessions.mkdir()
    result = run([a.fixture, sessions])
    print(result.stdout.strip())
    usable = 0
    for path in sorted(sessions.glob("*.flv")):
        info = json.loads(run([a.ffprobe, "-v", "error", "-show_streams", "-show_packets",
                               "-of", "json", path]).stdout)
        video = [x for x in info.get("packets", []) if x["codec_type"] == "video"]
        if not video:
            continue
        usable += 1
        require("K" in video[0]["flags"] and abs(float(video[0]["dts_time"])) < .002,
                "恢复文件没有从零点关键帧开始")
        require([s["codec_name"] for s in info["streams"]] == ["h264", "aac"], "恢复缺少编解码参数")
        for stream in info["streams"]:
            values = [float(x["dts_time"]) for x in info["packets"] if x["stream_index"] == stream["index"]]
            require(all(x < y for x, y in zip(values, values[1:])), "恢复后 DTS 回退")
        run([a.ffmpeg, "-v", "error", "-xerror", "-i", path, "-f", "null", "-"])
    require(usable >= 2, "没有产生至少两个可解码恢复会话")
    print(f"通过：{usable} 个真实 FLV 故障恢复产物可独立解码，首包 IDR 和时间原点正确")

    sample = root / "sample.mp4"
    run([a.ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=160x120:r=25:d=3",
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=3",
         "-c:v", "libx264", "-c:a", "aac", sample])

    def cmd(output, *args):
        return [a.program, "--input", str(sample), "--input", str(sample), "--width", "320",
                "--height", "240", "--output", str(output), "--duration-ms", "3000",
                "--output-timeout-ms", "200", "--retry-backoff-ms", "20", *args]

    # 本地输出走真实异步队列，但应背压而非丢包。
    local, stats = root / "local.flv", root / "output.csv"
    run(cmd(local, "--no-realtime", "--packet-queue-count", "4", "--packet-queue-bytes", "65536",
            "--stats-file", str(stats)))
    row = list(csv.DictReader(stats.open()))[-1]
    require(int(row["output_video"]) == 75 and int(row["output_audio"]) > 100, "本地异步输出丢失媒体")
    require(int(row["dropped_video"]) == 0 and int(row["dropped_audio"]) == 0, "本地输出误丢包")
    require(int(row["queue_packets"]) == 0 and int(row["queue_bytes"]) == 0, "正常收尾队列未排空")
    run([a.ffmpeg, "-v", "error", "-xerror", "-i", local, "-f", "null", "-"])
    print("通过：4包小队列本地背压，75视频帧无丢失，收尾统计归零")

    # 端口只bind不listen，避免与系统其他服务竞争同一个端口。
    with socket.socket() as unavailable:
        unavailable.bind(("127.0.0.1", 0))
        url = f"rtmp://127.0.0.1:{unavailable.getsockname()[1]}/live/test"
        start = time.monotonic()
        refused = run(cmd(url, "--reconnect-attempts", "2"), expected=1)
        require(refused.stderr.count("；等待 ") == 2, "重试次数没有遵守上限")
        require(time.monotonic() - start < 4, "连接拒绝未及时失败")
        disabled = run(cmd(url, "--reconnect-attempts", "0"), expected=1)
        require("；等待 " not in disabled.stderr, "关闭重连仍在重试")

        # EOF 后主线程正在等待输出结束，SIGINT 仍应打断长退避。
        process = subprocess.Popen(cmd(url, "--reconnect-attempts", "20",
                                       "--retry-backoff-ms", "4000", "--duration-ms", "200"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(.5)
            began = time.monotonic()
            process.send_signal(signal.SIGINT)
            _, errors = process.communicate(timeout=4)
            require(process.returncode == 130, errors)
            require(time.monotonic() - began < 1.5, "停止不能及时中断退避")
        finally:
            if process.poll() is None:
                process.kill(); process.communicate()
    print("通过：实际RTMP连接拒绝、重试上限、禁用重连及EOF等待期间停止")

    # TCP故障注入，仅接受连接保持沉默，没有RTMP协议实现或服务部署。
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(8)
        listener.settimeout(.1)
        stop = threading.Event()
        connections = []
        errors = []

        def silent_accept():
            try:
                while not stop.is_set():
                    try:
                        connection, _ = listener.accept()
                        connections.append(connection)
                    except socket.timeout:
                        pass
            except Exception as error:
                errors.append(error)

        worker = threading.Thread(target=silent_accept)
        worker.start()
        try:
            url = f"rtmp://127.0.0.1:{listener.getsockname()[1]}/live/test"
            began = time.monotonic()
            timeout = run(cmd(url, "--reconnect-attempts", "1"), expected=1)
            require("超时" in timeout.stderr and timeout.stderr.count("；等待 ") == 1,
                    "RTMP握手超时未分类或重试异常")
            require(time.monotonic() - began < 4, "握手阻塞未中断")
        finally:
            stop.set(); worker.join()
            for connection in connections:
                connection.close()
        require(not errors and len(connections) >= 2, "未实际建立重试TCP连接")
    print("通过：实际FFmpeg RTMP握手无响应超时及第二次TCP连接")

    run(cmd(root / "invalid.flv", "--packet-queue-bytes", "1"), expected=1)
    run(cmd("rtmp://127.0.0.1:1/live/test", "--no-realtime"), expected=1)
    print("注意：故障恢复传输为注入测试，不代表真实RTMP成功推流或端到端延迟验收")
